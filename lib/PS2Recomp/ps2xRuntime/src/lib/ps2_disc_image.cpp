#include "runtime/ps2_disc_image.h"

#include "ps2_runtime.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#if PS2X_HAS_CHD
#include <libchdr/chd.h>
#endif

namespace
{
    // (frame size, offset of the 2048 data bytes in the frame): cooked ISO,
    // then raw CD frames with and without subchannel, Mode 1 and Mode 2.
    constexpr std::pair<uint32_t, uint32_t> kPlainLayouts[] = {
        {2048, 0}, {2448, 0}, {2352, 16}, {2352, 24}, {2448, 16}, {2448, 24}};
    constexpr uint32_t kChdOffsets[] = {0, 16, 24};
    constexpr uint32_t kPrimaryVolumeLbn = 16;

    class PlainFrames final : public DiscImage::FrameSource
    {
    public:
        PlainFrames(const std::filesystem::path &path, uint32_t size) : m_file(path, std::ios::binary)
        {
            frameSize = size;
            std::error_code ec;
            const uint64_t bytes = std::filesystem::file_size(path, ec);
            frameCount = ec ? 0u : static_cast<uint32_t>(bytes / size);
        }

        bool readFrame(uint32_t frame, uint8_t *dst) override
        {
            if (frame >= frameCount)
                return false;
            m_file.clear();
            m_file.seekg(static_cast<std::streamoff>(frame) * frameSize);
            m_file.read(reinterpret_cast<char *>(dst), frameSize);
            return m_file.gcount() == static_cast<std::streamsize>(frameSize);
        }

    private:
        std::ifstream m_file;
    };

#if PS2X_HAS_CHD
    class ChdFrames final : public DiscImage::FrameSource
    {
    public:
        static std::unique_ptr<ChdFrames> open(const std::filesystem::path &path, std::string &error)
        {
            chd_file *chd = nullptr;
            const chd_error err = chd_open(path.string().c_str(), CHD_OPEN_READ, nullptr, &chd);
            if (err != CHDERR_NONE)
            {
                error = chd_error_string(err);
                return nullptr;
            }
            return std::unique_ptr<ChdFrames>(new ChdFrames(chd));
        }

        ~ChdFrames() override { chd_close(m_chd); }

        bool readFrame(uint32_t frame, uint8_t *dst) override
        {
            if (frame >= frameCount)
                return false;
            const uint32_t hunk = frame / m_framesPerHunk;
            if (hunk != m_cachedHunk)
            {
                if (chd_read(m_chd, hunk, m_hunk.data()) != CHDERR_NONE)
                    return false;
                m_cachedHunk = hunk;
            }
            std::memcpy(dst, m_hunk.data() + (frame % m_framesPerHunk) * frameSize, frameSize);
            return true;
        }

    private:
        explicit ChdFrames(chd_file *chd) : m_chd(chd)
        {
            const chd_header *header = chd_get_header(chd);
            frameSize = header->unitbytes;
            m_framesPerHunk = header->hunkbytes / header->unitbytes;
            m_hunk.resize(header->hunkbytes);
            frameCount = static_cast<uint32_t>(header->logicalbytes / header->unitbytes);

            // A CD-type CHD pads each track out to whole hunks; the first
            // track's own frame count is where the data volume ends.
            char meta[256] = {};
            uint32_t length = 0;
            if (chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, 0, meta, sizeof(meta) - 1,
                                 &length, nullptr, nullptr) == CHDERR_NONE)
            {
                if (const char *frames = std::strstr(meta, "FRAMES:"))
                    frameCount = static_cast<uint32_t>(std::strtoul(frames + 7, nullptr, 10));
            }
        }

        chd_file *m_chd;
        uint32_t m_framesPerHunk = 1;
        uint32_t m_cachedHunk = UINT32_MAX;
        std::vector<uint8_t> m_hunk;
    };
#endif

    bool hasVolumeDescriptor(DiscImage::FrameSource &frames, uint32_t offset)
    {
        std::vector<uint8_t> frame(frames.frameSize);
        return offset + 6 <= frames.frameSize &&
               frames.readFrame(kPrimaryVolumeLbn, frame.data()) &&
               std::memcmp(frame.data() + offset + 1, "CD001", 5) == 0;
    }

    uint32_t readLe32(const uint8_t *p)
    {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    bool namesMatch(const std::string &want, const uint8_t *name, size_t length)
    {
        size_t end = 0;
        while (end < length && name[end] != ';')
            ++end;
        if (end != want.size())
            return false;
        for (size_t i = 0; i < end; ++i)
        {
            if (std::toupper(name[i]) != std::toupper(static_cast<unsigned char>(want[i])))
                return false;
        }
        return true;
    }
}

std::unique_ptr<DiscImage> DiscImage::open(const std::filesystem::path &path, std::string &error)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
    {
        error = "file not found";
        return nullptr;
    }

    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });

    if (ext == ".chd")
    {
#if PS2X_HAS_CHD
        auto frames = ChdFrames::open(path, error);
        if (!frames)
            return nullptr;
        for (uint32_t offset : kChdOffsets)
        {
            if (hasVolumeDescriptor(*frames, offset))
                return std::unique_ptr<DiscImage>(new DiscImage(std::move(frames), offset));
        }
        error = "no ISO 9660 volume descriptor";
#else
        error = "built without CHD support";
#endif
        return nullptr;
    }

    for (const auto &[size, offset] : kPlainLayouts)
    {
        auto frames = std::make_unique<PlainFrames>(path, size);
        if (frames->frameCount > kPrimaryVolumeLbn && hasVolumeDescriptor(*frames, offset))
            return std::unique_ptr<DiscImage>(new DiscImage(std::move(frames), offset));
    }
    error = "no ISO 9660 volume descriptor";
    return nullptr;
}

DiscImage::DiscImage(std::unique_ptr<FrameSource> frames, uint32_t dataOffset)
    : m_frames(std::move(frames)), m_dataOffset(dataOffset)
{
}

bool DiscImage::readSectorLocked(uint32_t lbn, uint8_t *dst)
{
    if (m_frames->frameSize == kSectorSize)
        return m_frames->readFrame(lbn, dst);
    std::vector<uint8_t> frame(m_frames->frameSize);
    if (!m_frames->readFrame(lbn, frame.data()))
        return false;
    std::memcpy(dst, frame.data() + m_dataOffset, kSectorSize);
    return true;
}

bool DiscImage::readSectors(uint32_t lbn, uint32_t count, uint8_t *dst)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (!readSectorLocked(lbn + i, dst + static_cast<size_t>(i) * kSectorSize))
            return false;
    }
    return true;
}

size_t DiscImage::readExtent(const Extent &extent, uint64_t offset, uint8_t *dst, size_t size)
{
    if (offset >= extent.size)
        return 0;
    size = static_cast<size_t>(std::min<uint64_t>(size, extent.size - offset));

    std::lock_guard<std::mutex> lock(m_mutex);
    uint8_t sector[kSectorSize];
    size_t done = 0;
    while (done < size)
    {
        const uint64_t pos = offset + done;
        const uint32_t lbn = extent.lbn + static_cast<uint32_t>(pos / kSectorSize);
        const size_t within = static_cast<size_t>(pos % kSectorSize);
        const size_t chunk = std::min(size - done, kSectorSize - within);
        if (within == 0 && chunk == kSectorSize)
        {
            if (!readSectorLocked(lbn, dst + done))
                break;
        }
        else
        {
            if (!readSectorLocked(lbn, sector))
                break;
            std::memcpy(dst + done, sector + within, chunk);
        }
        done += chunk;
    }
    return done;
}

bool DiscImage::find(const std::string &path, Extent &out)
{
    std::string rest = path;
    if (const size_t colon = rest.find(':'); colon != std::string::npos)
        rest = rest.substr(colon + 1);
    if (const size_t version = rest.find(';'); version != std::string::npos)
        rest = rest.substr(0, version);
    std::replace(rest.begin(), rest.end(), '\\', '/');

    uint8_t pvd[kSectorSize];
    if (!readSectors(kPrimaryVolumeLbn, 1, pvd))
        return false;
    Extent current{readLe32(pvd + 156 + 2), readLe32(pvd + 156 + 10), true};

    size_t start = 0;
    while (start < rest.size())
    {
        size_t end = rest.find('/', start);
        if (end == std::string::npos)
            end = rest.size();
        const std::string part = rest.substr(start, end - start);
        start = end + 1;
        if (part.empty())
            continue;
        if (!current.isDir)
            return false;

        std::vector<uint8_t> dir(current.size);
        if (readExtent(current, 0, dir.data(), dir.size()) != dir.size())
            return false;

        bool found = false;
        for (size_t i = 0; i < dir.size();)
        {
            const uint8_t length = dir[i];
            if (length == 0)
            {
                // Records never cross a sector boundary; skip the padding.
                i = (i / kSectorSize + 1) * kSectorSize;
                continue;
            }
            if (i + 33 > dir.size() || i + 33 + dir[i + 32] > dir.size())
                break;
            if (namesMatch(part, &dir[i + 33], dir[i + 32]))
            {
                current = {readLe32(&dir[i + 2]), readLe32(&dir[i + 10]), (dir[i + 25] & 2) != 0};
                found = true;
                break;
            }
            i += length;
        }
        if (!found)
            return false;
    }

    out = current;
    return true;
}

DiscImage *ps2ConfiguredDisc()
{
    static std::mutex mutex;
    static std::filesystem::path openedPath;
    static std::unique_ptr<DiscImage> disc;

    std::lock_guard<std::mutex> lock(mutex);
    const std::filesystem::path &path = PS2Runtime::getIoPaths().cdImage;
    if (path.empty())
        return nullptr;
    if (path != openedPath)
    {
        openedPath = path;
        std::string error;
        disc = DiscImage::open(path, error);
        if (!disc)
            std::cerr << "disc image " << path << ": " << error << std::endl;
    }
    return disc.get();
}

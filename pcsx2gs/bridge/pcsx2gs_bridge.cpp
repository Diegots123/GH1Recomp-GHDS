#include "pcsx2gs_bridge.h"

#include "GS/GS.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "Config.h"
#include "common/AlignedMalloc.h"
#include "common/FileSystem.h"
#include "pcsx2gs_host.h"

#include <cstring>

namespace pcsx2gs
{
    namespace
    {
        u8 *s_regs = nullptr; // the GS's view of the privileged registers
        bool s_open = false;
    }

    bool open(const NativeWindow &window, const Settings &settings, std::string &error)
    {
        WindowInfo wi;
        wi.type = window.type == NativeWindow::Type::Win32 ? WindowInfo::Type::Win32 : WindowInfo::Type::X11;
        wi.display_connection = window.display;
        wi.window_handle = window.handle;
        wi.surface_width = window.width;
        wi.surface_height = window.height;
        wi.surface_scale = window.scale;
        host().acquireWindow = [wi](bool) { return std::optional<WindowInfo>(wi); };

        // PCSX2 keeps the shaders it compiles in its cache folder; without
        // one, every effect is compiled again the first time each run shows
        // it, and the picture stalls for that moment.
        if (!settings.cacheDir.empty() && FileSystem::EnsureDirectoryExists(settings.cacheDir.c_str(), true))
            EmuFolders::Cache = settings.cacheDir;

        Pcsx2Config::GSOptions options;
        options.UpscaleMultiplier = settings.upscale;
        // A PS2 on a 4:3 television, and nothing on top of the picture.
        options.AspectRatio = AspectRatioType::R4_3;
        EmuConfig.GS = options;
        EmuConfig.CurrentAspectRatio = AspectRatioType::R4_3;

        if (!s_regs)
            s_regs = static_cast<u8 *>(_aligned_malloc(0x2000, 32));
        std::memset(s_regs, 0, 0x2000);
        const GSRendererType renderer = settings.null       ? GSRendererType::Null
                                        : settings.software ? GSRendererType::SW
                                                            : GSRendererType::OGL;
        if (!GSopen(options, renderer, s_regs, settings.vsync ? GSVSyncMode::FIFO : GSVSyncMode::Disabled, false))
        {
            error = "the GS could not open its renderer (OpenGL 4.3 or newer is needed)";
            return false;
        }
        s_open = true;
        return true;
    }

    void close()
    {
        if (s_open)
            GSclose();
        s_open = false;
    }

    void transfer(uint32_t path, const uint8_t *data, uint32_t qwc)
    {
        if (!s_open || qwc == 0)
            return;
        // Each path keeps its own GIFtag state, as on the GIF.
        switch (path)
        {
        case 1:
            g_gs_renderer->Transfer<0>(data, qwc);
            break;
        case 2:
            g_gs_renderer->Transfer<1>(data, qwc);
            break;
        default:
            g_gs_renderer->Transfer<2>(data, qwc);
            break;
        }
    }

    void vsync(const uint8_t *privRegs)
    {
        if (!s_open)
            return;
        std::memcpy(s_regs, privRegs, 0x2000);
        u32 csr;
        std::memcpy(&csr, s_regs + 0x1000, sizeof(csr));
        // CSR.FIELD (bit 13) is the field now being displayed, as PCSX2's MTGS passes it.
        GSvsync((csr & 0x2000) ? 0 : 1, true);
    }

    void readLocal(uint8_t *dst, uint32_t qwc)
    {
        if (!s_open)
        {
            std::memset(dst, 0, static_cast<size_t>(qwc) * 16);
            return;
        }
        GSInitAndReadFIFO(dst, qwc);
    }

    void resize(uint32_t width, uint32_t height, float scale)
    {
        if (s_open)
            GSResizeDisplayWindow(width, height, scale);
    }

    bool snapshot(std::vector<uint32_t> &rgba, uint32_t &width, uint32_t &height)
    {
        if (!s_open)
            return false;
        u32 w = 0, h = 0;
        if (!GSSaveSnapshotToMemory(0, 0, true, false, &w, &h, &rgba))
            return false;
        width = w;
        height = h;
        return true;
    }
}

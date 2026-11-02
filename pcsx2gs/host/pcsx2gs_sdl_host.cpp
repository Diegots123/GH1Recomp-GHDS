#include "pcsx2gs_sdl_host.h"

#include "pcsx2gs_bridge.h"
#include "guitars.h"
#include "keyboard_options.h"
#include "runtime/gs/gs_null_backend.h"
#include "runtime/gs/gs_packet_tap.h"
#include "runtime/host_hooks.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace pcsx2gs
{
    namespace
    {
        // Port 1 as the host last decided it: bit 0 plugged, bit 1 a guitar.
        std::atomic<uint32_t> s_portOne{3u};
        // The screen GH1 shows (GH1's side says so).
        std::atomic<Screen> s_screen{Screen::Other};

        // PS2 pad bits, active low (libpad order).
        constexpr uint16_t kSelect = 0x0001, kL3 = 0x0002, kR3 = 0x0004, kStart = 0x0008;
        constexpr uint16_t kUp = 0x0010, kRight = 0x0020, kDown = 0x0040, kLeft = 0x0080;
        constexpr uint16_t kL2 = 0x0100, kR2 = 0x0200, kL1 = 0x0400, kR1 = 0x0800;
        constexpr uint16_t kTriangle = 0x1000, kCircle = 0x2000, kCross = 0x4000, kSquare = 0x8000;

        constexpr uint32_t kPrivRegsSize = 0x2000;
        constexpr uint32_t kFramesAhead = 2; // vsyncs the EE may queue before it waits, as PCSX2

        enum RecordType : uint32_t
        {
            kGif = 1,
            kVSync = 2,
            kRead = 3,
        };

        struct Record
        {
            uint32_t type;
            uint32_t path;
            uint32_t bytes;
            uint32_t pad;
        };
        static_assert(sizeof(Record) == 16);

        struct ReadRequest
        {
            uint8_t *dst;
            uint32_t qwc;
            bool done;
        };

        void putReg(uint8_t *regs, uint32_t offset, uint64_t value)
        {
            std::memcpy(regs + offset, &value, sizeof(value));
        }

        // The runtime's privileged GS registers in the PS2's own layout.
        void privRegsPs2Layout(const GSRegisters &gs, uint8_t *out)
        {
            std::memset(out, 0, kPrivRegsSize);
            putReg(out, 0x000, gs.pmode);
            putReg(out, 0x010, gs.smode1);
            putReg(out, 0x020, gs.smode2);
            putReg(out, 0x030, gs.srfsh);
            putReg(out, 0x040, gs.synch1);
            putReg(out, 0x050, gs.synch2);
            putReg(out, 0x060, gs.syncv);
            putReg(out, 0x070, gs.dispfb1);
            putReg(out, 0x080, gs.display1);
            putReg(out, 0x090, gs.dispfb2);
            putReg(out, 0x0A0, gs.display2);
            putReg(out, 0x0B0, gs.extbuf);
            putReg(out, 0x0C0, gs.extdata);
            putReg(out, 0x0D0, gs.extwrite);
            putReg(out, 0x0E0, gs.bgcolor);
            putReg(out, 0x1000, gs.csr.load(std::memory_order_acquire));
            putReg(out, 0x1010, gs.imr);
            putReg(out, 0x1040, gs.busdir);
            putReg(out, 0x1080, gs.siglblid);
        }

        float envFloat(const char *name, float fallback)
        {
            const char *v = std::getenv(name);
            return v && *v ? static_cast<float>(std::atof(v)) : fallback;
        }

        bool envFlag(const char *name)
        {
            const char *v = std::getenv(name);
            return v && *v && std::strcmp(v, "0") != 0;
        }

        // A test script of button presses by presented frame, so runs are the
        // same however fast the host draws: "1500=cross;2400=start+r2", each
        // press held for 8 frames.
        // A step of GH1_SCRIPT: "<frame>=a+b" holds buttons for 8 frames,
        // "<from>-<to>=..." for that span. "whammy" works the whammy and
        // "flip" makes port 1 the other kind for a while (tests).
        struct ScriptStep
        {
            uint64_t frame;
            uint16_t buttons;
            uint64_t until = 0;
            bool whammy = false;
            bool flip = false;
        };

        std::vector<ScriptStep> parseScript(const char *text)
        {
            static const struct
            {
                const char *name;
                uint16_t bit;
            } kNames[] = {{"up", kUp}, {"down", kDown}, {"left", kLeft}, {"right", kRight},
                          {"cross", kCross}, {"circle", kCircle}, {"square", kSquare},
                          {"triangle", kTriangle}, {"l1", kL1}, {"r1", kR1}, {"l2", kL2},
                          {"r2", kR2}, {"l3", kL3}, {"r3", kR3}, {"start", kStart}, {"select", kSelect}};
            std::vector<ScriptStep> steps;
            std::string all = text ? text : "";
            size_t at = 0;
            while (at < all.size())
            {
                size_t end = all.find(';', at);
                if (end == std::string::npos)
                    end = all.size();
                const std::string step = all.substr(at, end - at);
                at = end + 1;
                const size_t eq = step.find('=');
                if (eq == std::string::npos)
                    continue;
                char *rest = nullptr;
                ScriptStep st{std::strtoull(step.c_str(), &rest, 10), 0};
                st.until = (rest && *rest == '-') ? std::strtoull(rest + 1, nullptr, 10) : st.frame + 8u;
                std::string names = step.substr(eq + 1) + "+";
                size_t n = 0;
                while (n < names.size())
                {
                    const size_t plus = names.find('+', n);
                    const std::string name = names.substr(n, plus - n);
                    n = plus + 1;
                    for (const auto &k : kNames)
                        if (name == k.name)
                            st.buttons |= k.bit;
                    st.whammy |= name == "whammy";
                    st.flip |= name == "flip";
                }
                steps.push_back(st);
            }
            return steps;
        }

        class SdlHost final : public PS2Runtime::HostFrontend
        {
        public:
            explicit SdlHost(const HostConfig &config) : m_config(config) {}

            bool initialize(PS2Runtime &runtime, const char *title) override
            {
                m_runtime = &runtime;
                if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
                {
                    std::fprintf(stderr, "[host] SDL_Init: %s\n", SDL_GetError());
                    return false;
                }
                // The community controller database (SDL_GameControllerDB)
                // beside the exe: the layouts of many more USB adapters for
                // the DualShock 2 than SDL itself knows.
                if (const char *base = SDL_GetBasePath())
                {
                    const int added =
                        SDL_AddGamepadMappingsFromFile((std::string(base) + "gamecontrollerdb.txt").c_str());
                    if (added > 0)
                        std::fprintf(stderr, "[host] gamecontrollerdb.txt: %d layouts\n", added);
                }
                const auto &options = runtime.hostOptions();
                SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
                if (options.hidden)
                    flags |= SDL_WINDOW_HIDDEN;
                else if (m_config.fullscreen)
                    flags |= SDL_WINDOW_FULLSCREEN;
                m_window = SDL_CreateWindow(title, static_cast<int>(m_config.windowWidth),
                                            static_cast<int>(m_config.windowHeight), flags);
                if (!m_window)
                {
                    std::fprintf(stderr, "[host] SDL_CreateWindow: %s\n", SDL_GetError());
                    return false;
                }

                NativeWindow nw;
                const SDL_PropertiesID props = SDL_GetWindowProperties(m_window);
#if defined(_WIN32)
                nw.type = NativeWindow::Type::Win32;
                nw.handle = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#else
                nw.type = NativeWindow::Type::X11;
                nw.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
                nw.handle = reinterpret_cast<void *>(static_cast<uintptr_t>(
                    SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0)));
                if (!nw.display)
                {
                    std::fprintf(stderr, "[host] the window is not an X11 window (set SDL_VIDEO_DRIVER=x11)\n");
                    return false;
                }
#endif
                int pw = 0, ph = 0;
                SDL_GetWindowSizeInPixels(m_window, &pw, &ph);
                nw.width = static_cast<uint32_t>(pw);
                nw.height = static_cast<uint32_t>(ph);
                nw.scale = SDL_GetWindowDisplayScale(m_window);

                Settings settings;
                settings.software = m_config.software;
                settings.null = m_config.null;
                settings.upscale = m_config.upscale;
                settings.vsync = m_config.vsync;
                if (const char *base = SDL_GetBasePath())
                    settings.cacheDir = std::string(base) + "cache";
                std::string error;
                if (!pcsx2gs::open(nw, settings, error))
                {
                    std::fprintf(stderr, "[host] %s\n", error.c_str());
                    return false;
                }

                // The runtime's own rasterizer stops; the GS gets every packet.
                runtime.gs().setRasterBackend(std::make_unique<GSNullBackend>(
                    [this](uint8_t *dst, uint32_t maxBytes) { return readLocal(dst, maxBytes); }));
                ps2x::gs::setPacketTap([this](uint32_t path, const uint8_t *data, uint32_t bytes)
                                       { push(kGif, path, data, bytes); });
                ps2x::host::setVBlankHook([this](uint64_t) { vblank(); });
                ps2x::host::setPadSource([this](int port, int slot, uint8_t *data, size_t size)
                                         { return readPad(port, slot, data, size); });

                m_shotFrom = [] {
                    const char *v = std::getenv("PS2X_SHOT_FROM");
                    return v ? std::strtoull(v, nullptr, 10) : 0ull;
                }();
                m_statsStart = std::chrono::steady_clock::now();
                m_script = parseScript(std::getenv("GH1_SCRIPT"));
                if (const char *v = std::getenv("GH1_DISPTRACE"))
                    std::sscanf(v, "%llu-%llu", reinterpret_cast<unsigned long long *>(&m_traceFrom),
                                reinterpret_cast<unsigned long long *>(&m_traceTo));
                if (const char *v = std::getenv("GH1_EXIT_AT"))
                    m_exitAt = std::strtoull(v, nullptr, 10);
                return true;
            }

            bool frame(PS2Runtime &runtime) override
            {
                if (!pumpEvents() || (m_exitAt != 0 && m_frames >= m_exitAt))
                {
                    stop();
                    return false;
                }
                {
                    std::unique_lock lock(m_mutex);
                    if (m_pending.empty())
                        m_work.wait_for(lock, std::chrono::milliseconds(4));
                    m_draining.swap(m_pending);
                }
                const uint32_t vsyncs = process(m_draining);
                m_draining.clear();
                if (vsyncs != 0)
                {
                    {
                        std::lock_guard lock(m_mutex);
                        m_queuedVsyncs -= std::min(m_queuedVsyncs, vsyncs);
                    }
                    m_space.notify_all();
                }
                (void)runtime;
                return true;
            }

            void shutdown(PS2Runtime &) override
            {
                stop();
                ps2x::host::setVBlankHook({});
                ps2x::host::setPadSource({});
                ps2x::gs::setPacketTap({});
                pcsx2gs::close();
                for (guitars::Device &d : m_devices)
                    guitars::close(d);
                m_devices.clear();
                m_options.close();
                if (m_window)
                    SDL_DestroyWindow(m_window);
                m_window = nullptr;
                SDL_Quit();
            }

        private:
            // --- EE side --------------------------------------------------------

            void push(uint32_t type, uint32_t path, const void *data, uint32_t bytes)
            {
                {
                    std::lock_guard lock(m_mutex);
                    const size_t at = m_pending.size();
                    m_pending.resize(at + sizeof(Record) + ((bytes + 15u) & ~15u));
                    const Record rec{type, path, bytes, 0};
                    std::memcpy(m_pending.data() + at, &rec, sizeof(rec));
                    if (bytes)
                        std::memcpy(m_pending.data() + at + sizeof(rec), data, bytes);
                }
                m_work.notify_one();
            }

            void vblank()
            {
                uint8_t regs[kPrivRegsSize];
                privRegsPs2Layout(m_runtime->memory().gs(), regs);
                std::unique_lock lock(m_mutex);
                const size_t at = m_pending.size();
                m_pending.resize(at + sizeof(Record) + kPrivRegsSize);
                const Record rec{kVSync, 0, kPrivRegsSize, 0};
                std::memcpy(m_pending.data() + at, &rec, sizeof(rec));
                std::memcpy(m_pending.data() + at + sizeof(rec), regs, kPrivRegsSize);
                ++m_queuedVsyncs;
                m_work.notify_one();
                // Too far ahead of the picture: the EE waits for the GS.
                while (m_queuedVsyncs > kFramesAhead && !m_stopping && !m_runtime->isStopRequested())
                    m_space.wait_for(lock, std::chrono::milliseconds(10));
            }

            uint32_t readLocal(uint8_t *dst, uint32_t maxBytes)
            {
                ReadRequest req{dst, maxBytes / 16u, false};
                const ReadRequest *ptr = &req;
                push(kRead, 0, &ptr, sizeof(ptr));
                std::unique_lock lock(m_mutex);
                while (!req.done && !m_stopping && !m_runtime->isStopRequested())
                    m_space.wait_for(lock, std::chrono::milliseconds(10));
                return req.done ? req.qwc * 16u : 0u;
            }

            bool readPad(int port, int slot, uint8_t *data, size_t size)
            {
                if (port != 0 || slot != 0 || size < 32)
                    return false;
                const uint64_t s = m_pad.load(std::memory_order_acquire);
                std::memset(data, 0, 32);
                data[0] = 0x01;
                data[1] = 0x73;
                data[2] = static_cast<uint8_t>(s & 0xFF);
                data[3] = static_cast<uint8_t>((s >> 8) & 0xFF);
                data[4] = static_cast<uint8_t>(s >> 16); // right x
                data[5] = static_cast<uint8_t>(s >> 24); // right y
                data[6] = static_cast<uint8_t>(s >> 32); // left x
                data[7] = static_cast<uint8_t>(s >> 40); // left y
                return true;
            }

            void stop()
            {
                {
                    std::lock_guard lock(m_mutex);
                    m_stopping = true;
                }
                m_space.notify_all();
            }

            // --- host side ------------------------------------------------------

            uint32_t process(const std::vector<uint8_t> &buffer)
            {
                uint32_t vsyncs = 0;
                size_t at = 0;
                while (at + sizeof(Record) <= buffer.size())
                {
                    Record rec;
                    std::memcpy(&rec, buffer.data() + at, sizeof(rec));
                    const uint8_t *payload = buffer.data() + at + sizeof(rec);
                    at += sizeof(rec) + ((rec.bytes + 15u) & ~15u);
                    switch (rec.type)
                    {
                    case kGif:
                        if (tracing())
                            traceFrames(rec.path, payload, rec.bytes);
                        pcsx2gs::transfer(rec.path, payload, rec.bytes / 16u);
                        break;
                    case kVSync:
                        if (tracing())
                            traceVsync(payload);
                        pcsx2gs::vsync(payload);
                        ++vsyncs;
                        presented();
                        break;
                    case kRead:
                    {
                        ReadRequest *req;
                        std::memcpy(&req, payload, sizeof(req));
                        pcsx2gs::readLocal(req->dst, req->qwc);
                        {
                            std::lock_guard lock(m_mutex);
                            req->done = true;
                        }
                        m_space.notify_all();
                        break;
                    }
                    default:
                        break;
                    }
                }
                return vsyncs;
            }

            bool tracing() const { return m_traceTo != 0 && m_frames >= m_traceFrom && m_frames <= m_traceTo; }

            // GH1_DISPTRACE=<from>-<to>: per frame, the display registers at
            // VBlank and the framebuffers (FRAME_1/2 FBP) drawn to before it.
            void traceFrames(uint32_t path, const uint8_t *data, uint32_t bytes)
            {
                // FNV-1a of each path's data this frame, and its size.
                uint64_t &h = m_traceHash[path & 3u];
                if (h == 0)
                    h = 1469598103934665603ull;
                for (uint32_t i = 0; i < bytes; ++i)
                    h = (h ^ data[i]) * 1099511628211ull;
                m_traceBytes[path & 3u] += bytes;
                // GH1_DISPDUMP=<dir>: each traced frame's packets, {u32 path, u32 bytes, data}.
                static const char *dumpDir = std::getenv("GH1_DISPDUMP");
                if (dumpDir)
                {
                    char name[512];
                    std::snprintf(name, sizeof(name), "%s/gif_%06llu.bin", dumpDir, (unsigned long long)m_frames);
                    if (std::FILE *f = std::fopen(name, "ab"))
                    {
                        std::fwrite(&path, 4, 1, f);
                        std::fwrite(&bytes, 4, 1, f);
                        std::fwrite(data, 1, bytes, f);
                        std::fclose(f);
                    }
                }
                ++m_tracePackets[path & 3u];
                uint32_t at = 0;
                while (at + 16u <= bytes)
                {
                    uint64_t lo, hi;
                    std::memcpy(&lo, data + at, 8);
                    std::memcpy(&hi, data + at + 8, 8);
                    at += 16u;
                    const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
                    const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
                    uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
                    if (nreg == 0u)
                        nreg = 16u;
                    if (flg == 0u)
                    {
                        for (uint32_t i = 0; i < nloop * nreg && at + 16u <= bytes; ++i, at += 16u)
                        {
                            if (((hi >> ((i % nreg) * 4u)) & 0xFu) != 0xEu)
                                continue;
                            uint64_t v, a;
                            std::memcpy(&v, data + at, 8);
                            std::memcpy(&a, data + at + 8, 8);
                            const uint32_t reg = static_cast<uint32_t>(a & 0xFFu);
                            char item[48];
                            item[0] = 0;
                            if (reg == 0x4C || reg == 0x4D) // FRAME_1/2: FBP, FBW, PSM
                                std::snprintf(item, sizeof(item), "F%u:%x/%u/%x", path, unsigned(v & 0x1FF),
                                              unsigned((v >> 16) & 0x3F), unsigned((v >> 24) & 0x3F));
                            else if (reg == 0x4E || reg == 0x4F) // ZBUF_1/2: ZBP, PSM, ZMSK
                                std::snprintf(item, sizeof(item), "Z%u:%x/%x/%u", path, unsigned(v & 0x1FF),
                                              unsigned((v >> 24) & 0xF), unsigned((v >> 32) & 1));
                            else if (reg == 0x06 || reg == 0x07) // TEX0_1/2: TBP0, TBW, PSM
                                std::snprintf(item, sizeof(item), "T%u:%x/%u/%x", path, unsigned(v & 0x3FFF),
                                              unsigned((v >> 14) & 0x3F), unsigned((v >> 20) & 0x3F));
                            else if (reg == 0x40 || reg == 0x41) // SCISSOR
                                std::snprintf(item, sizeof(item), "S%u:%u-%u,%u-%u", path, unsigned(v & 0x7FF),
                                              unsigned((v >> 16) & 0x7FF), unsigned((v >> 32) & 0x7FF), unsigned((v >> 48) & 0x7FF));
                            else if (reg == 0x18 || reg == 0x19) // XYOFFSET
                                std::snprintf(item, sizeof(item), "O%u:%u,%u", path, unsigned(v & 0xFFFF) >> 4,
                                              unsigned((v >> 32) & 0xFFFF) >> 4);
                            else if (reg == 0x47 || reg == 0x48) // TEST
                                std::snprintf(item, sizeof(item), "E%u:%llx", path, (unsigned long long)(v & 0x7FFFF));
                            else if (reg == 0x50) // BITBLTBUF: DBP, DBW, DPSM
                                std::snprintf(item, sizeof(item), "U%u:%x/%u/%x", path, unsigned((v >> 32) & 0x3FFF),
                                              unsigned((v >> 48) & 0x3F), unsigned((v >> 56) & 0x3F));
                            if (item[0])
                                m_traceItems[item]++;
                        }
                    }
                    else if (flg == 1u)
                        at += ((nloop * nreg + 1u) / 2u) * 16u;
                    else
                        at += nloop * 16u;
                }
            }

            void traceVsync(const uint8_t *regs)
            {
                auto reg = [&](uint32_t off) { uint64_t v; std::memcpy(&v, regs + off, 8); return v; };
                std::string fbps;
                for (uint32_t p = 1; p <= 3; ++p)
                {
                    char t[96];
                    std::snprintf(t, sizeof(t), " P%u:%up/%ub/%016llx", p, m_tracePackets[p], m_traceBytes[p],
                                  (unsigned long long)m_traceHash[p]);
                    fbps += t;
                    m_traceHash[p] = 0;
                    m_traceBytes[p] = 0;
                    m_tracePackets[p] = 0;
                }
                for (const auto &[item, n] : m_traceItems)
                    fbps += " " + item + "x" + std::to_string(n);
                std::printf("[disp] %llu pmode %llx smode2 %llx dispfb1 %llx display1 %llx dispfb2 %llx display2 %llx csr %llx drew%s\n",
                            static_cast<unsigned long long>(m_frames),
                            (unsigned long long)reg(0x0), (unsigned long long)reg(0x20), (unsigned long long)reg(0x70),
                            (unsigned long long)reg(0x80), (unsigned long long)reg(0x90), (unsigned long long)reg(0xA0),
                            (unsigned long long)reg(0x1000), fbps.c_str());
                m_traceItems.clear();
            }

            void presented()
            {
                ++m_frames;
                const auto &options = m_runtime->hostOptions();
                if (options.shotEvery != 0 && m_frames % options.shotEvery == 0 && m_frames >= m_shotFrom)
                    shot();
                if (m_config.stats)
                {
                    const auto now = std::chrono::steady_clock::now();
                    const double secs = std::chrono::duration<double>(now - m_statsStart).count();
                    if (secs >= 5.0)
                    {
                        std::printf("[gs] %.1f fps\n", (m_frames - m_statsFrames) / secs);
                        std::fflush(stdout);
                        m_statsStart = now;
                        m_statsFrames = m_frames;
                    }
                }
            }

            void shot()
            {
                std::vector<uint32_t> rgba;
                uint32_t w = 0, h = 0;
                if (!pcsx2gs::snapshot(rgba, w, h) || rgba.empty())
                    return;
                SDL_Surface *surface = SDL_CreateSurfaceFrom(static_cast<int>(w), static_cast<int>(h),
                                                             SDL_PIXELFORMAT_RGBA32, rgba.data(),
                                                             static_cast<int>(w * 4));
                if (!surface)
                    return;
                char name[32];
                std::snprintf(name, sizeof(name), "frame_%06llu.png", static_cast<unsigned long long>(m_frames));
                SDL_SavePNG(surface, (m_runtime->hostOptions().shotDir / name).string().c_str());
                SDL_DestroySurface(surface);
            }

            bool pumpEvents()
            {
                bool keep = true;
                SDL_Event ev;
                while (SDL_PollEvent(&ev))
                {
                    if (m_options.handle(ev))
                        continue; // the options window's own
                    switch (ev.type)
                    {
                    case SDL_EVENT_QUIT:
                    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                        keep = false;
                        break;
                    case SDL_EVENT_KEY_DOWN:
                        // F11 or Alt+Enter: borderless fullscreen on the desktop, and back.
                        if (!ev.key.repeat && (ev.key.key == SDLK_F11 ||
                                               (ev.key.key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT))))
                        {
                            const bool full = (SDL_GetWindowFlags(m_window) & SDL_WINDOW_FULLSCREEN) != 0;
                            SDL_SetWindowFullscreen(m_window, !full);
                        }
                        // F1: the keyboard options, on the main menu only.
                        if (!ev.key.repeat && ev.key.key == SDLK_F1 &&
                            s_screen.load(std::memory_order_acquire) == Screen::MainMenu)
                            m_options.toggle(m_window);
                        break;
                    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                        if (ev.window.windowID == SDL_GetWindowID(m_window))
                            pcsx2gs::resize(static_cast<uint32_t>(ev.window.data1),
                                            static_cast<uint32_t>(ev.window.data2), SDL_GetWindowDisplayScale(m_window));
                        break;
                    case SDL_EVENT_JOYSTICK_ADDED:
                    {
                        // Every controller, guitars that are not SDL gamepads included.
                        guitars::Device d;
                        if (guitars::open(ev.jdevice.which, d))
                            m_devices.push_back(d);
                        break;
                    }
                    case SDL_EVENT_JOYSTICK_REMOVED:
                        for (auto it = m_devices.begin(); it != m_devices.end(); ++it)
                        {
                            if (it->id == ev.jdevice.which)
                            {
                                guitars::close(*it);
                                m_devices.erase(it);
                                break;
                            }
                        }
                        break;
                    default:
                        break;
                    }
                }
                updatePad();
                if (m_options.isOpen() && s_screen.load(std::memory_order_acquire) != Screen::MainMenu)
                    m_options.close(); // the game left the main menu
                m_options.draw();
                return keep;
            }

            void updatePad()
            {
                uint16_t pressed = 0;
                int lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;

                // A PS2 guitar on a PS2-to-USB adapter ("Twin USB Joystick")
                // is the adapter's gamepad, holding D-Left as every PS2 guitar
                // does, from the moment it is there: a gamepad that holds it
                // from its first second for kAdapterGuitarFrames is that
                // guitar, and its buttons and sticks pass through as is. One
                // that does not, or lets go, is a DualShock for good (so a
                // DualShock's D-Left never makes it a guitar).
                for (guitars::Device &d : m_devices)
                {
                    if (d.guitar || !d.gamepad || d.notGuitar)
                        continue;
                    ++d.age;
                    if (SDL_GetGamepadButton(d.gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))
                    {
                        if (d.leftHeld < kAdapterGuitarFrames)
                            ++d.leftHeld;
                    }
                    else if (d.leftHeld >= kAdapterGuitarFrames || d.age > kAdapterGraceFrames)
                    {
                        d.notGuitar = true;
                    }
                    else
                    {
                        d.leftHeld = 0;
                    }
                }
                auto adapterGuitar = [](const guitars::Device &d)
                { return !d.guitar && !d.notGuitar && d.leftHeld >= kAdapterGuitarFrames; };

                // What port 1 is. The keyboard turned on plays before any
                // controller; off, it plays only while none is connected.
                // Otherwise a guitar when one is connected, else a DualShock.
                const keyboard::Config &kb = m_config.keyboard;
                bool anyGuitar = false, anyPad = false;
                for (const guitars::Device &d : m_devices)
                    (d.guitar || adapterGuitar(d) ? anyGuitar : anyPad) = true;
                const Screen screen = s_screen.load(std::memory_order_acquire);
                // At start the keyboard is on exactly when no controller is
                // connected (controllers that show up in the first seconds,
                // as SDL finds them, count too); F1 changes it afterwards.
                const bool starting = m_frames < kStartupFrames;
                if (starting && !m_config.keyboardForced)
                    m_config.keyboard.on = !anyGuitar && !anyPad;
                const bool keyboardPlays = kb.on || (!anyGuitar && !anyPad);
                // Port 1 takes a new kind only on the main menu (and once at
                // start): elsewhere, a controller plugged in or out, or the
                // keyboard's settings, leave it as it is, and the keyboard
                // plays as the kind the port has.
                bool wantGuitar = m_config.guitar && (keyboardPlays ? !kb.noStrum : anyGuitar);
                // GH1_TEST_FLIP=1 (tests): F9 makes port 1 the other kind for
                // a few seconds, wherever the game is, as a controller swapped
                // and swapped back.
                static const bool testFlip = std::getenv("GH1_TEST_FLIP") != nullptr;
                bool scriptFlip = false;
                for (const ScriptStep &st : m_script)
                    scriptFlip |= st.flip && m_frames >= st.frame && m_frames < st.until;
                if (testFlip)
                {
                    const bool *k = SDL_GetKeyboardState(nullptr);
                    if (k[SDL_SCANCODE_F9] && m_testFlipUntil + 30u < m_frames)
                        m_testFlipUntil = m_frames + 150u;
                }
                if (scriptFlip || m_frames < m_testFlipUntil)
                    wantGuitar = !wantGuitar;
                const bool guitarPort =
                    (!m_identityKnown || starting || screen == Screen::MainMenu || testFlip || !m_script.empty())
                        ? wantGuitar
                        : m_guitarPort;
                if (!m_identityKnown || guitarPort != m_guitarPort)
                {
                    // A new kind of controller: unplugged for a moment, so the
                    // game looks at it again (not at boot, before it has looked).
                    if (m_identityKnown || !guitarPort)
                        m_unplugUntil = m_frames + kUnplugFrames;
                    m_guitarPort = guitarPort;
                    m_identityKnown = true;
                    std::fprintf(stderr, "[host] port 1: %s\n", guitarPort ? "guitar" : "DualShock 2");
                }
                s_portOne.store((m_frames >= m_unplugUntil ? 1u : 0u) | (guitarPort ? 2u : 0u),
                                std::memory_order_release);

                // Held whammy keys move the whammy the way a hand works the
                // bar or the stick: from rest to full and back, five times a
                // second.
                const uint64_t phase = m_frames % 12u;
                const float wave = static_cast<float>(phase < 6u ? phase : 12u - phase) / 6.0f;
                float whammy = 0.0f;
                bool keyWhammy = false;

                // A key held while the game goes between a song and the menus
                // counts again only once it is let go: Enter that resumes from
                // the pause menu must not then press Start in the song.
                const bool inSong = screen == Screen::Song;
                const bool *keys = SDL_GetKeyboardState(nullptr);
                if (inSong != m_keysInSong)
                {
                    m_keysInSong = inSong;
                    for (int code = 0; code < SDL_SCANCODE_COUNT; ++code)
                        m_keyLatched[code] = keys[code];
                }
                for (int code = 0; code < SDL_SCANCODE_COUNT; ++code)
                    m_keyLatched[code] = m_keyLatched[code] && keys[code];
                // Keys pressed on the options screen are its own: the Enter
                // that closes it must not then confirm in the game's menu.
                // (It may have closed in this frame's events, with the key
                // still down: the frame after counts too.)
                if (m_options.isOpen() || m_optionsWereOpen)
                    for (int code = 0; code < SDL_SCANCODE_COUNT; ++code)
                        m_keyLatched[code] = m_keyLatched[code] || keys[code];
                m_optionsWereOpen = m_options.isOpen();

                if (keyboardPlays && !m_options.isOpen())
                {
                    // Alt+Enter is fullscreen, not a button.
                    const bool alt = (SDL_GetModState() & SDL_KMOD_ALT) != 0;
                    auto down = [&](int code)
                    {
                        return code > 0 && code < SDL_SCANCODE_COUNT && keys[code] && !m_keyLatched[code] &&
                               !(alt && code == SDL_SCANCODE_RETURN);
                    };
                    if (inSong)
                    {
                        if (guitarPort)
                        {
                            static constexpr uint16_t kBits[keyboard::kStrumKeyCount] = {
                                kR2, kCircle, kTriangle, kCross, kSquare, kUp, kDown, kStart, kSelect, 0};
                            for (int k = 0; k < keyboard::kStrumKeyCount; ++k)
                                if (down(kb.strum[k]))
                                    pressed |= kBits[k];
                            keyWhammy = down(kb.strum[keyboard::kStrumWhammy]);
                        }
                        else
                        {
                            static constexpr uint16_t kBits[keyboard::kPadKeyCount] = {
                                kL2, kL1, kR1, kR2, kCross, kStart, kSelect, 0};
                            for (int k = 0; k < keyboard::kPadKeyCount; ++k)
                                if (down(kb.pad[k]))
                                    pressed |= kBits[k];
                            keyWhammy = down(kb.pad[keyboard::kPadWhammy]);
                        }
                    }
                    else
                    {
                        // The menus: confirm and back are what the port's kind
                        // selects and goes back with (green and red on a
                        // guitar; X and triangle on a DualShock).
                        const uint16_t kBits[keyboard::kMenuKeyCount] = {
                            kUp, kDown, kLeft, kRight, guitarPort ? kR2 : kCross,
                            guitarPort ? kCircle : kTriangle, kStart};
                        for (int k = 0; k < keyboard::kMenuKeyCount; ++k)
                            if (down(kb.menu[k]))
                                pressed |= kBits[k];
                    }
                    if (keyWhammy)
                        whammy = wave;
                }

                // Controllers, unless the keyboard plays before them: guitars
                // as the PS2 guitar; other gamepads as a DualShock 2, its
                // buttons where SDL puts them (fixed: no rebinding).
                const guitars::Device *sticksFrom = nullptr; // the gamepad last used
                for (guitars::Device &d : m_devices)
                {
                    if (kb.on)
                        break;
                    if (d.guitar)
                    {
                        const guitars::State st = guitars::read(d);
                        pressed |= st.buttons;
                        whammy = std::max(whammy, st.whammy);
                        continue;
                    }
                    SDL_Gamepad *pad = d.gamepad;
                    uint16_t mine = 0;
                    auto btn = [&](SDL_GamepadButton b, uint16_t bit)
                    {
                        if (SDL_GetGamepadButton(pad, b))
                            mine |= bit;
                    };
                    btn(SDL_GAMEPAD_BUTTON_SOUTH, kCross);
                    btn(SDL_GAMEPAD_BUTTON_EAST, kCircle);
                    btn(SDL_GAMEPAD_BUTTON_WEST, kSquare);
                    btn(SDL_GAMEPAD_BUTTON_NORTH, kTriangle);
                    btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kL1);
                    btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kR1);
                    btn(SDL_GAMEPAD_BUTTON_BACK, kSelect);
                    btn(SDL_GAMEPAD_BUTTON_START, kStart);
                    btn(SDL_GAMEPAD_BUTTON_LEFT_STICK, kL3);
                    btn(SDL_GAMEPAD_BUTTON_RIGHT_STICK, kR3);
                    btn(SDL_GAMEPAD_BUTTON_DPAD_UP, kUp);
                    btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDown);
                    btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT, kLeft);
                    btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kRight);
                    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384)
                        mine |= kL2;
                    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384)
                        mine |= kR2;
                    pressed |= mine;
                    // The sticks come from the gamepad whose buttons were
                    // pressed last: a two-port adapter shows up as two
                    // gamepads, and the empty port's sticks may sit anywhere.
                    if (mine & ~(adapterGuitar(d) ? kLeft : 0))
                        d.lastPress = m_frames + 1u;
                    if (d.lastPress != 0u && (!sticksFrom || d.lastPress > sticksFrom->lastPress))
                        sticksFrom = &d;
                }
                if (sticksFrom)
                {
                    static constexpr SDL_GamepadAxis kAxes[4] = {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY,
                                                                 SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY};
                    int *sticks[4] = {&lx, &ly, &rx, &ry};
                    for (int i = 0; i < 4; ++i)
                        *sticks[i] = std::clamp((SDL_GetGamepadAxis(sticksFrom->gamepad, kAxes[i]) + 32768) >> 8, 0, 255);
                }
                if (guitarPort && !(sticksFrom && adapterGuitar(*sticksFrom)))
                {
                    // PCSX2's PadGuitar: whammy on the left stick's Y, 0x7f at
                    // rest to 0x00, the rest of the sticks at 0x7f.
                    lx = rx = ry = 0x7f;
                    ly = 0x7f - static_cast<int>(whammy * 127.0f + 0.5f);
                }
                else if (keyWhammy)
                {
                    ly = 0x80 - static_cast<int>(whammy * 128.0f + 0.5f); // the left stick worked up
                }

                for (const ScriptStep &st : m_script)
                    if (m_frames >= st.frame && m_frames < st.until)
                    {
                        pressed |= st.buttons;
                        if (st.whammy)
                            ly = guitarPort ? 0x7f - static_cast<int>(wave * 127.0f) : 0x80 - static_cast<int>(wave * 128.0f);
                    }

                if (guitarPort)
                    pressed |= kLeft; // a guitar holds D-Left

                // GH1_HOSTPAD=1 (tests): what port 1 sends, when it changes.
                static const bool tracePad = std::getenv("GH1_HOSTPAD") != nullptr;
                if (tracePad)
                {
                    const uint64_t now = uint64_t(pressed) | (uint64_t(ly) << 16) | (uint64_t(inSong) << 24) |
                                         (uint64_t(guitarPort) << 25) | (uint64_t(keyboardPlays) << 26) |
                                         (uint64_t(keyWhammy) << 27);
                    if (now != m_lastPadTrace)
                        std::fprintf(stderr, "[host pad] frame %llu pressed %04x ly %02x song %d guitar %d kb %d whammy %d\n",
                                     static_cast<unsigned long long>(m_frames), pressed, ly, inSong, guitarPort,
                                     keyboardPlays, keyWhammy);
                    m_lastPadTrace = now;
                }

                const uint16_t buttons = static_cast<uint16_t>(~pressed);
                const uint64_t s = uint64_t(buttons) | (uint64_t(rx) << 16) | (uint64_t(ry) << 24) |
                                   (uint64_t(lx) << 32) | (uint64_t(ly) << 40);
                m_pad.store(s, std::memory_order_release);
            }

            HostConfig m_config;
            keyboard::OptionsWindow m_options{m_config.keyboard, m_config.iniPath};
            PS2Runtime *m_runtime = nullptr;
            SDL_Window *m_window = nullptr;
            std::vector<guitars::Device> m_devices;
            bool m_keysInSong = false;
            bool m_optionsWereOpen = false;
            uint64_t m_testFlipUntil = 0;
            uint64_t m_lastPadTrace = 0;
            std::array<bool, SDL_SCANCODE_COUNT> m_keyLatched{};
            static constexpr uint64_t kUnplugFrames = 30;
            static constexpr uint64_t kStartupFrames = 120; // 2 s: controllers SDL finds at start
            static constexpr uint32_t kAdapterGuitarFrames = 120; // 2 s
            static constexpr uint32_t kAdapterGraceFrames = 60;   // its first second
            bool m_identityKnown = false;
            bool m_guitarPort = true;
            uint64_t m_unplugUntil = 0;
            std::atomic<uint64_t> m_pad{0x80808080FFFFull};

            std::mutex m_mutex;
            std::condition_variable m_work;  // EE -> host: records queued
            std::condition_variable m_space; // host -> EE: vsyncs drained, reads done
            std::vector<uint8_t> m_pending;
            std::vector<uint8_t> m_draining;
            uint32_t m_queuedVsyncs = 0;
            bool m_stopping = false;

            uint64_t m_frames = 0;
            uint64_t m_exitAt = 0;
            uint64_t m_traceFrom = 0, m_traceTo = 0;
            std::map<std::string, uint32_t> m_traceItems;
            uint64_t m_traceHash[4] = {};
            uint32_t m_traceBytes[4] = {};
            uint32_t m_tracePackets[4] = {};
            std::vector<ScriptStep> m_script;
            uint64_t m_shotFrom = 0;
            uint64_t m_statsFrames = 0;
            std::chrono::steady_clock::time_point m_statsStart;
        };
    }

    void setScreen(Screen screen) { s_screen.store(screen, std::memory_order_release); }

    PortOne portOne()
    {
        const uint32_t v = s_portOne.load(std::memory_order_acquire);
        return {(v & 1u) != 0u, (v & 2u) != 0u};
    }

    HostConfig hostConfigFromEnvironment()
    {
        HostConfig c;
        auto window = [&c](const char *w) {
            unsigned ww = 0, wh = 0;
            if (std::sscanf(w, "%ux%u", &ww, &wh) == 2 && ww >= 320 && wh >= 240)
            {
                c.windowWidth = ww;
                c.windowHeight = wh;
            }
        };
        auto flag = [](const std::string &v) { return !v.empty() && v != "0" && v != "false" && v != "no"; };
        auto trim = [](const std::string &v) {
            const size_t a = v.find_first_not_of(" \t\r\n");
            const size_t b = v.find_last_not_of(" \t\r\n");
            return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
        };
        auto lower = [](std::string v) {
            for (char &ch : v)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return v;
        };

        // GH1Recomp.ini beside the exe: the settings before any [section];
        // the keyboard's (keyboard.h) from all of it.
        std::string text;
        if (const char *base = SDL_GetBasePath())
        {
            c.iniPath = std::string(base) + "GH1Recomp.ini";
            std::ifstream f(keyboard::utf8Path(c.iniPath), std::ios::binary);
            std::ostringstream all;
            all << f.rdbuf();
            text = all.str();
        }
        std::istringstream in(text);
        std::string line;
        bool inSection = false;
        while (std::getline(in, line))
        {
            if (const size_t hash = line.find('#'); hash != std::string::npos)
                line.erase(hash);
            line = trim(line);
            if (line.size() >= 2 && line.front() == '[' && line.back() == ']')
                inSection = true;
            const size_t eq = line.find('=');
            if (inSection || eq == std::string::npos)
                continue;
            const std::string key = lower(trim(line.substr(0, eq)));
            const std::string value = trim(line.substr(eq + 1));
            if (key == "upscale")
                c.upscale = std::clamp(static_cast<float>(std::atof(value.c_str())), 1.0f, 8.0f);
            else if (key == "renderer")
                c.software = value == "sw";
            else if (key == "vsync")
                c.vsync = flag(value);
            else if (key == "window")
                window(value.c_str());
            else if (key == "fullscreen")
                c.fullscreen = flag(value);
        }
        std::string problems;
        keyboard::read(text, c.keyboard, problems);
        if (!problems.empty())
        {
            const std::string message = "GH1Recomp.ini: estas teclas nao foram aceitas; essa parte do teclado "
                                        "fica com as teclas padrao (F1 no jogo para mudar).\n\n" + problems;
            std::fprintf(stderr, "[host] %s", message.c_str());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "GH1 Recomp", message.c_str(), nullptr);
        }

        c.upscale = std::clamp(envFloat("GH1_UPSCALE", c.upscale), 1.0f, 8.0f);
        if (const char *r = std::getenv("GH1_RENDERER"))
        {
            c.software = std::strcmp(r, "sw") == 0;
            c.null = std::strcmp(r, "null") == 0;
        }
        if (std::getenv("GH1_VSYNC"))
            c.vsync = envFlag("GH1_VSYNC");
        if (std::getenv("GH1_FULLSCREEN"))
            c.fullscreen = envFlag("GH1_FULLSCREEN");
        c.stats = envFlag("GH1_STATS");
        c.guitar = !envFlag("GH1_JOYPAD");
        // GH1_KEYBOARD=strum|nostrum: the keyboard on, in that mode (tests).
        if (const char *k = std::getenv("GH1_KEYBOARD"))
        {
            c.keyboard.on = true;
            c.keyboardForced = true;
            c.keyboard.noStrum = std::strcmp(k, "nostrum") == 0 || std::strcmp(k, "dualshock") == 0;
        }
        if (const char *w = std::getenv("GH1_WINDOW"))
            window(w);
        return c;
    }

    std::unique_ptr<PS2Runtime::HostFrontend> makeSdlHost(const HostConfig &config)
    {
        return std::make_unique<SdlHost>(config);
    }
}

#pragma once

// The standalone PCSX2 GS behind a plain interface, so the host side (SDL,
// the PS2Recomp runtime) never includes PCSX2's headers. Every call is made
// on one thread: the one that called open(), which owns the GPU context.

#include <cstdint>
#include <string>
#include <vector>

namespace pcsx2gs
{
    struct NativeWindow
    {
        enum class Type
        {
            X11,
            Win32,
        };
        Type type = Type::X11;
        void *display = nullptr; // X11 Display*
        void *handle = nullptr;  // X11 Window id, or HWND
        uint32_t width = 0;
        uint32_t height = 0;
        float scale = 1.0f;
    };

    struct Settings
    {
        bool software = false;  // PCSX2's software renderer instead of OpenGL
        bool null = false;      // draw nothing (measuring everything else)
        float upscale = 1.0f;   // internal resolution, multiples of native
        bool vsync = false;     // present on the host's vertical blank
        std::string cacheDir;   // where compiled shaders are kept between runs (UTF-8)
    };

    bool open(const NativeWindow &window, const Settings &settings, std::string &error);
    void close();

    // One GIF packet (whole qwords) from PATH1, 2 or 3.
    void transfer(uint32_t path, const uint8_t *data, uint32_t qwc);
    // VBlank start: the privileged registers as the guest left them (0x2000
    // bytes, PS2 layout from 0x12000000), then the field is shown.
    void vsync(const uint8_t *privRegs);
    // A local-to-host transfer set up by TRXDIR = 1, read into dst.
    void readLocal(uint8_t *dst, uint32_t qwc);
    void resize(uint32_t width, uint32_t height, float scale);
    // The last presented frame as RGBA8, at the GS's internal resolution
    // with the display's aspect applied.
    bool snapshot(std::vector<uint32_t> &rgba, uint32_t &width, uint32_t &height);
}

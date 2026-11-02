#pragma once

// The PS2Recomp runtime's host side on SDL3 with PCSX2's GS: an SDL window
// the GS draws into (OpenGL), keyboard and gamepad input, and the GS fed on
// the host thread from a queue the EE side fills (like PCSX2's MTGS).

#include "keyboard.h"
#include "ps2_runtime.h"

#include <array>
#include <memory>
#include <string>

namespace pcsx2gs
{
    struct HostConfig
    {
        float upscale = 1.0f;     // internal resolution, multiples of native
        bool software = false;    // PCSX2's software renderer
        bool null = false;        // no drawing (GH1_RENDERER=null, for measuring)
        bool vsync = false;       // present on the host's vertical blank
        uint32_t windowWidth = 1280;
        uint32_t windowHeight = 960;
        bool fullscreen = false;  // borderless on the desktop; F11 or Alt+Enter toggles
        bool stats = false;       // print presented frames per second
        bool guitar = true;       // false: port 1 is always a DualShock 2 (GH1_JOYPAD=1)
        keyboard::Config keyboard; // on/off, mode and keys (F1 changes them)
        bool keyboardForced = false; // on/off set by GH1_KEYBOARD, not by what is connected at start
        std::string iniPath;       // GH1Recomp.ini beside the exe
    };

    // Port 1 as the host has it now: plugged (false for a moment when it
    // changes kind) and a guitar or a DualShock 2.
    struct PortOne
    {
        bool plugged;
        bool guitar;
    };
    PortOne portOne();

    // The screen GH1 shows, as GH1's side reports it. In a song (not paused)
    // the keyboard plays with its game keys, elsewhere with its menu keys.
    // Port 1 changes kind (and F1 opens) only on the main menu.
    enum class Screen
    {
        Other,
        MainMenu,
        Song,
    };
    void setScreen(Screen screen);

    // Reads HostConfig from GH1Recomp.ini beside the exe (key = value lines:
    // upscale, renderer = hw|sw, vsync, window = <w>x<h>, fullscreen, and
    // the keyboard's, see keyboard.h), then
    // from the environment, which wins: GH1_UPSCALE, GH1_RENDERER=sw|null,
    // GH1_VSYNC=1, GH1_FULLSCREEN=1, GH1_STATS=1, GH1_JOYPAD=1 (port 1 a
    // DualShock, not a guitar), GH1_WINDOW=<w>x<h>.
    HostConfig hostConfigFromEnvironment();

    std::unique_ptr<PS2Runtime::HostFrontend> makeSdlHost(const HostConfig &config);
}

// GH1 Recomp's game layer: everything installed into the runtime once the
// recompiled GH1 executable is loaded. Only what the PC needs in place of PS2
// hardware goes here (IOP modules, audio, video, input, VU1); nothing that changes
// how GH1 plays. Two extras Diego asked for: venue = off (opted into in
// the ini) and the disc's two hidden songs in Quick Play.

#include "addresses.h"
#include "pad.h"
#include "synth.h"
#include "trace.h"
#include "venue.h"
#include "extra_songs.h"
#include "flares.h"
#include "movie_audio.h"
#include "vu1.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#if defined(GH1_PCSX2GS)
#include "pcsx2gs_sdl_host.h"
#endif

#include <chrono>
#include <cstdlib>
#include <cstring>

// Before the window opens: PCSX2's GS in an SDL window when it is built in
// (GH1_RAYLIB=1 keeps the runtime's software GS, for comparisons).
namespace
{
    // venue = off in GH1Recomp.ini (or GH1_VENUE=off): the one extra that
    // changes what the game shows, chosen by the player (see venue.cpp).
    bool s_venue = true;
    // audio_offset in GH1Recomp.ini (or GH1_AUDIO_OFFSET): audio calibration.
    int s_audioOffsetMs = 0;
}

void ps2xConfigureHost(PS2Runtime &runtime)
{
#if defined(GH1_PCSX2GS)
    const pcsx2gs::HostConfig config = pcsx2gs::hostConfigFromEnvironment();
    s_venue = config.venue;
    s_audioOffsetMs = config.audioOffsetMs;
    const char *raylib = std::getenv("GH1_RAYLIB");
    if (!raylib || !*raylib || *raylib == '0')
        runtime.setHostFrontend(pcsx2gs::makeSdlHost(config));
#else
    if (const char *venue = std::getenv("GH1_VENUE"))
        s_venue = std::strcmp(venue, "off") != 0;
    if (const char *offset = std::getenv("GH1_AUDIO_OFFSET"))
        s_audioOffsetMs = std::atoi(offset);
#endif
    gh1::installVu1(runtime);
}

void ps2xInstallGame(PS2Runtime &runtime)
{
    gh1::installSynth(runtime, gh1::kRetail, s_audioOffsetMs);
    gh1::installPad(runtime, gh1::kRetail);
    gh1::installMovieAudio(runtime);
    gh1::installTraces(runtime);
    gh1::installVenue(runtime, s_venue);
    gh1::installExtraSongs(runtime);
    gh1::installFlares(runtime);
    // GH1_VBLANK_HZ=<hz> (developers, measuring): VBlanks at that rate instead
    // of NTSC's, so the game runs as fast as its work per frame lets it.
    if (const char *hz = std::getenv("GH1_VBLANK_HZ"))
        if (const double rate = std::atof(hz); rate > 0.0)
            runtime.eeScheduler().setVBlankPeriod(std::chrono::nanoseconds(static_cast<int64_t>(1e9 / rate)));
}

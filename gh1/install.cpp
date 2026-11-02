// GH1 Recomp's game layer: everything installed into the runtime once the
// recompiled GH1 executable is loaded. Only what the PC needs in place of PS2
// hardware goes here (IOP modules, audio, video, input, VU1); nothing that changes
// how GH1 plays.

#include "addresses.h"
#include "pad.h"
#include "synth.h"
#include "trace.h"
#include "vu1.h"

#include "ps2_runtime.h"

#if defined(GH1_PCSX2GS)
#include "pcsx2gs_sdl_host.h"
#endif

#include <cstdlib>

// Before the window opens: PCSX2's GS in an SDL window when it is built in
// (GH1_RAYLIB=1 keeps the runtime's software GS, for comparisons).
void ps2xConfigureHost(PS2Runtime &runtime)
{
#if defined(GH1_PCSX2GS)
    const char *raylib = std::getenv("GH1_RAYLIB");
    if (!raylib || !*raylib || *raylib == '0')
        runtime.setHostFrontend(pcsx2gs::makeSdlHost(pcsx2gs::hostConfigFromEnvironment()));
#endif
    gh1::installVu1(runtime);
}

void ps2xInstallGame(PS2Runtime &runtime)
{
    gh1::installSynth(runtime, gh1::kRetail);
    gh1::installPad(runtime, gh1::kRetail);
    gh1::installTraces(runtime);
}

#pragma once

#include "addresses.h"
#include "ps2_runtime.h"

namespace gh1
{
    // HMX_synth_s, the IOP synth module GH1 loads as SYNTH_S.IRX, served on
    // the host by GH Classic's implementation of the same module.
    void installSynth(PS2Runtime &runtime, const Addresses &addresses);
}

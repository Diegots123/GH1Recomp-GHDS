#pragma once

#include "addresses.h"

class PS2Runtime;

namespace gh1
{
    // Port 1 is a RedOctane guitar or a DualShock 2, as the host decides.
    void installPad(PS2Runtime &runtime, const Addresses &addresses);
}

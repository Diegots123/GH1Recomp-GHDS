#pragma once

// Addresses in GH1 retail (SLUS-21224, boot ELF SHA-1 8f1e0368...) that the
// game layer hooks or reads. Each is found by code, not guessed; the GH2
// retail counterpart (config/gh2-retail.symbols) is noted beside it.

#include <cstdint>

namespace gh1
{
    struct Addresses
    {
        // CtlClientPoll__Fv (GH2 0x22dc68). GH1's ControlClient.cpp has the
        // same five functions in the same order and sizes as GH2's: Copy 96,
        // CtlClientInit 104 (0x219a58, binds SID 0x75433178 in a loop),
        // CtlClientCall(int) 32, CtlClientCall(int, const void *, int) 120,
        // CtlClientPoll 136.
        uint32_t ctlClientPoll;

        // CtlServerInit (0x21a980, matched exactly) stores the reply handler
        // with `sw $a2, 0x1d28(0x2f0000)` and registers its RPC server with
        // the receive buffer `0x380000 - 0x6840` (GH2: 0x3de454, 0x484340).
        uint32_t synthServerHandler;
        uint32_t synthServerBuffer;

        // scePadInfoAct and scePadGetState (libpad, SDK hash matches;
        // tools/gh1_stub_gate.py).
        uint32_t scePadInfoAct;
        uint32_t scePadGetState;

        // UIScreen::Enter(UIScreen *from) (placed by order beside GH2's,
        // 0x208878): `this` is the screen the game goes to, its name a
        // const char * at +0x24 ("game" while a song plays, "pause" over it;
        // seen with GH1_TRACE=ui).
        uint32_t uiScreenEnter;
        uint32_t screenNameOffset;
    };

    inline constexpr Addresses kRetail{
        .ctlClientPoll = 0x219b58u,
        .synthServerHandler = 0x2f1d28u,
        .synthServerBuffer = 0x3797c0u,
        .scePadInfoAct = 0x2738c8u,
        .scePadGetState = 0x273788u,
        .uiScreenEnter = 0x208878u,
        .screenNameOffset = 0x24u,
    };
}

// venue = off (GH1Recomp.ini): nothing is drawn behind the track, for weak
// GPUs. An extra the player opts into (venue = on is the default), and the
// one place GH1 Recomp changes what the game shows: the stage, the band and
// the crowd are not drawn, on a black background. The game runs exactly as
// ever, its world included (only that panel's drawing is skipped), so song
// timing, input and scoring do not change.

#include "venue.h"

#include "guest.h"
#include "hook.h"
#include "ps2_runtime_macros.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace gh1
{
    namespace
    {
        // The song screen ("game") draws six panels in order (UIScreen::Draw,
        // 0x208af0: the panel list at +0x2c, each panel's vtable at +4 and
        // its Draw at vtable +0x64; GH1_TRACE=venue lists them):
        //   0x2f6618 GamePanel   UIPanel::Draw, no directory: nothing
        //   0x2f69f8 (world)     0x10d488: the venue, band and crowd
        //   0x2f8b70 TrackPanel  0x15bba8: the track and the gems
        //   0x2f7ef0, 0x2f81b0, 0x2f6be0: score, rock meter, star power,
        //            the song's title
        // The world panel's Draw runs UIPanel::Draw (it has no directory)
        // and then draws the world object at 0x363748 (0x1696d8, 0x169818).
        // Seen with it skipped: a black background, everything else as ever;
        // the GS's own time per frame (llvmpipe, 1x) fell from 27 to 4-8 ms.
        constexpr uint32_t kWorldPanelDraw = 0x10d488u;
        constexpr uint32_t kUiScreenDraw = 0x208af0u;

        struct WorldDrawTag;
        bool skipWorld(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            gh2::leave(ctx);
            return true;
        }

        // GH1_TRACE=venue: each panel a screen draws, the first time.
        struct ScreenDrawTag;
        void onScreenDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t self = GPR_U32(ctx, 4);
            const uint32_t sentinel = gh2::load<uint32_t>(rdram, self + 0x2cu + 4u);
            static std::set<uint64_t> seen;
            uint32_t node = gh2::load<uint32_t>(rdram, sentinel);
            for (int n = 0; node != sentinel && n < 64; ++n, node = gh2::load<uint32_t>(rdram, node))
            {
                const uint32_t panel = gh2::load<uint32_t>(rdram, node + 8u);
                const uint32_t vtable = gh2::load<uint32_t>(rdram, panel + 4u);
                if (seen.insert((uint64_t(self) << 32) | panel).second)
                    std::fprintf(stderr, "[gh1 venue] screen %x panel %x vtable %x draw %x\n", self, panel, vtable,
                                 gh2::load<uint32_t>(rdram, vtable + 0x64u));
            }
        }
    }

    void installVenue(PS2Runtime &runtime, bool show)
    {
        if (!show)
        {
            gh2::Override<WorldDrawTag>::install(runtime, kWorldPanelDraw, &skipWorld);
            std::fprintf(stderr, "[gh1] venue = off: the stage, band and crowd are not drawn\n");
        }
        const char *trace = std::getenv("GH1_TRACE");
        if (trace && std::strstr(trace, "venue"))
            gh2::EntryHook<ScreenDrawTag>::install(runtime, kUiScreenDraw, &onScreenDraw);
    }
}

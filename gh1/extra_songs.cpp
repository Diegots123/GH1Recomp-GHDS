// GH1's two hidden songs in Quick Play. Both are on the disc, chart and
// audio complete, but no list names them: Graveyard Shift
// (songs/graveyardshift) and Trippolette (songs/advharmony, after the
// harmony project it came from). Added at Diego's request, always on:
// their entries go into the game's own song list as it is loaded, and Quick
// Play's list gets them at its end. Career and the store are as ever.
//
// What the disc does not have is written here: the names on screen, tempo
// (the MIDI's first, which also picks the animation tempo as the other songs
// do: 130 bpm and up is kTempoFast), the preview (the first chorus, 30 s), and
// Quick Play's character, guitar and venue. The audio settings are the ones
// most songs use; both songs have the usual four channels.

#include "extra_songs.h"

#include "guest.h"
#include "hook.h"
#include "ps2_runtime_macros.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace gh1
{
    namespace
    {
        // Matched exactly from GH2's names (work/gh1-retail.status) unless noted.
        constexpr uint32_t kConfigRoot = 0x2f1d70u;     // SystemConfig()'s DataArray *, set by InitSystem
        constexpr uint32_t kDataVariable = 0x235930u;   // DataVariable(Symbol): the first call after
                                                        // InitSystem stores the root (fuzzy 0.96)
        constexpr uint32_t kDataReadString = 0x230798u; // DataReadString(const char *)
        constexpr uint32_t kSystemConfigSym = 0x224270u; // SystemConfig(Symbol)
        constexpr uint32_t kInsertNodes = 0x22d430u;    // DataArray::InsertNodes(int, const DataArray *)
        constexpr uint32_t kGetQuickplaySongs = 0x137d58u; // Campaign::GetQuickplaySongs(vector<DataArray *> &) const
        // The STL pool allocator GetQuickplaySongs grows its vector with
        // (inlined there): alloc(bytes, name), free(bytes, ptr), the name
        // from 0x29b348 when memory tracking (0x2f1ef0) is on, else 0x2f1ef4.
        constexpr uint32_t kPoolAlloc = 0x23f728u;
        constexpr uint32_t kPoolFree = 0x23f768u;
        constexpr uint32_t kAllocNameOn = 0x2f1ef0u;
        constexpr uint32_t kAllocName = 0x2f1ef4u;
        constexpr uint32_t kAllocTypeName = 0x29b348u;

        // DataArray: nodes at +0 (8 bytes each: value, type), size (short) at +8.
        constexpr uint32_t kTypeSymbol = 5u, kTypeArray = 0x10u;

        const char *const kSongs =
            "(graveyardshift"
            " (name \"Graveyard Shift\")"
            " (artist \"Unknown\")"
            " (song (name songs/graveyardshift/graveyardshift) (tracks 1) (slip_tracks ((2 3)))"
            "  (pans (-1.0 1.0 -1.0 1.0)) (vols (0.9 0.9 0.9 0.9)) (cores (-1 -1 1 1)) (solo (riffs standard)))"
            " (bank sfx/song_default)"
            " (bpm 215)"
            " (anim_tempo kTempoFast)"
            " (preview 21074 51074)"
            " (midi_file songs/graveyardshift/graveyardshift.mid)"
            " (quickplay (character punk) (guitar gibson_sg) (venue small_club)))"
            "(advharmony"
            " (name \"Trippolette\")"
            " (artist \"Andrew Buch\")"
            " (song (name songs/advharmony/advharmony) (tracks 1) (slip_tracks ((2 3)))"
            "  (pans (-1.0 1.0 -1.0 1.0)) (vols (0.9 0.9 0.9 0.9)) (cores (-1 -1 1 1)) (solo (riffs standard)))"
            " (bank sfx/song_default)"
            " (bpm 206)"
            " (anim_tempo kTempoFast)"
            " (preview 36699 66699)"
            " (midi_file songs/advharmony/advharmony.mid)"
            " (quickplay (character alterna) (guitar gibson_lespaul) (venue theatre)))";

        bool s_added = false;
        bool s_trace = false;
        std::vector<uint32_t> s_entries; // the two songs' DataArrays, once in the list

        uint32_t node(uint8_t *rdram, uint32_t array, uint32_t index, uint32_t *type)
        {
            const uint32_t nodes = gh2::load<uint32_t>(rdram, array);
            *type = gh2::load<uint32_t>(rdram, nodes + index * 8u + 4u);
            return gh2::load<uint32_t>(rdram, nodes + index * 8u);
        }
        int16_t size(uint8_t *rdram, uint32_t array) { return gh2::load<int16_t>(rdram, array + 8u); }

        // The child array of `list` whose first node is the symbol `name`.
        uint32_t findEntry(uint8_t *rdram, uint32_t list, uint32_t name)
        {
            for (int i = 0; i < size(rdram, list); ++i)
            {
                uint32_t type = 0;
                const uint32_t child = node(rdram, list, static_cast<uint32_t>(i), &type);
                if (type != kTypeArray || size(rdram, child) < 1)
                    continue;
                uint32_t headType = 0;
                if (node(rdram, child, 0, &headType) == name && headType == kTypeSymbol)
                    return child;
            }
            return 0;
        }

        // Guest bytes below the stack for a string the guest reads, the
        // stack pointer moved past them while the guest runs.
        uint32_t pushString(uint8_t *rdram, R5900Context *ctx, const char *text)
        {
            const uint32_t length = static_cast<uint32_t>(std::strlen(text)) + 1u;
            const uint32_t at = (GPR_U32(ctx, 29) - length - 64u) & ~15u;
            std::memcpy(getMemPtr(rdram, at), text, length);
            SET_GPR_U32(ctx, 29, at - 64u);
            return at;
        }

        // The entries into the game's song list, right after InitSystem
        // loads the configuration and before anything reads it (the high
        // scores list one per song from it, so these get theirs).
        struct AddTag;
        void addSongs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (s_added || gh2::load<uint32_t>(rdram, kConfigRoot) == 0u)
                return;
            s_added = true;
            const gh2::EntryArgs args(ctx);
            const uint32_t sp = GPR_U32(ctx, 29);

            const uint32_t parsed = static_cast<uint32_t>(
                runtime->callGuestFunction(rdram, ctx, kDataReadString, {pushString(rdram, ctx, kSongs)}));
            SET_GPR_U32(ctx, 29, sp);
            const uint32_t key = static_cast<uint32_t>(
                runtime->callGuestFunction(rdram, ctx, kDataReadString, {pushString(rdram, ctx, "songs")}));
            SET_GPR_U32(ctx, 29, sp);
            uint32_t type = 0;
            const uint32_t songsSymbol = key && size(rdram, key) > 0 ? node(rdram, key, 0, &type) : 0u;
            if (!parsed || size(rdram, parsed) != 2 || type != kTypeSymbol)
            {
                std::fprintf(stderr, "[gh1] hidden songs: could not read their entries\n");
                args.restore(ctx);
                return;
            }
            const uint32_t list =
                static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, kSystemConfigSym, {songsSymbol}));
            if (!list)
            {
                std::fprintf(stderr, "[gh1] hidden songs: no song list\n");
                args.restore(ctx);
                return;
            }
            const int16_t before = size(rdram, list);
            // Each only once, should the disc's list ever have it.
            uint32_t fresh = 0;
            for (uint32_t i = 0; i < 2; ++i)
            {
                uint32_t t = 0, ht = 0;
                const uint32_t entry = node(rdram, parsed, i, &t);
                const uint32_t name = node(rdram, entry, 0, &ht);
                if (const uint32_t existing = findEntry(rdram, list, name))
                    s_entries.push_back(existing);
                else
                    ++fresh;
            }
            if (fresh == 2)
            {
                runtime->callGuestFunction(rdram, ctx, kInsertNodes,
                                           {list, static_cast<uint32_t>(size(rdram, list)), parsed});
                for (uint32_t i = 0; i < 2; ++i)
                {
                    uint32_t t = 0;
                    s_entries.push_back(node(rdram, parsed, i, &t));
                }
            }
            std::fprintf(stderr, "[gh1] hidden songs: Graveyard Shift and Trippolette in Quick Play (%d songs)\n",
                         size(rdram, list));
            if (s_trace)
            {
                // anim_tempo as the disc's songs have it and as ours parsed.
                for (uint32_t entry : {node(rdram, list, static_cast<uint32_t>(before - 1), &type),
                                       s_entries.empty() ? 0u : s_entries.front()})
                    for (int i = 0; entry && i < size(rdram, entry); ++i)
                    {
                        uint32_t ct = 0;
                        const uint32_t child = node(rdram, entry, static_cast<uint32_t>(i), &ct);
                        if (ct != kTypeArray || size(rdram, child) < 2)
                            continue;
                        uint32_t t0 = 0, t1 = 0;
                        const uint32_t head = node(rdram, child, 0, &t0);
                        const uint32_t value = node(rdram, child, 1, &t1);
                        char name[32] = {};
                        for (int c = 0; c < 31 && t0 == kTypeSymbol; ++c)
                            if (!(name[c] = static_cast<char>(gh2::load<uint8_t>(rdram, head + c))))
                                break;
                        std::fprintf(stderr, "[gh1 songs] %x %s: type %x value %x\n", entry, name, t1, value);
                    }
            }
            args.restore(ctx);
        }

        // Appends a word to an STLport vector of 4-byte items (begin +0, end
        // +4, capacity +0xc) unless it is there, growing it as the game's
        // own code does, with the STL pool allocator. False when that fails.
        bool appendWord(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t vector, uint32_t value)
        {
            uint32_t begin = gh2::load<uint32_t>(rdram, vector), end = gh2::load<uint32_t>(rdram, vector + 4u);
            const uint32_t capacity = gh2::load<uint32_t>(rdram, vector + 0xcu);
            for (uint32_t at = begin; at < end; at += 4u)
                if (gh2::load<uint32_t>(rdram, at) == value)
                    return true;
            if (end == capacity)
            {
                const uint32_t count = (end - begin) / 4u;
                const uint32_t grown = count ? count * 2u : 1u;
                uint32_t name = gh2::load<uint32_t>(rdram, kAllocName);
                if (gh2::load<uint32_t>(rdram, kAllocNameOn) != 0u)
                    name = gh2::load<uint32_t>(
                        rdram, static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, kAllocTypeName, {})));
                const uint32_t fresh =
                    static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, kPoolAlloc, {grown * 4u, name}));
                if (!fresh)
                    return false;
                if (count)
                    std::memmove(getMemPtr(rdram, fresh), getMemPtr(rdram, begin), count * 4u);
                if (begin)
                    runtime->callGuestFunction(rdram, ctx, kPoolFree, {capacity - begin, begin});
                begin = fresh;
                end = fresh + count * 4u;
                gh2::store<uint32_t>(rdram, vector, begin);
                gh2::store<uint32_t>(rdram, vector + 0xcu, fresh + grown * 4u);
            }
            gh2::store<uint32_t>(rdram, end, value);
            gh2::store<uint32_t>(rdram, vector + 4u, end + 4u);
            return true;
        }

        // Quick Play's list as the game makes it, then the hidden songs at
        // its end (vector<DataArray *>: begin +0, end +4, capacity +0xc).
        struct QuickplayTag;
        bool quickplaySongs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t self = GPR_U32(ctx, 4), vector = GPR_U32(ctx, 5);
            runtime->callGuestFunction(rdram, ctx, kGetQuickplaySongs, {self, vector},
                                       gh2::Override<QuickplayTag>::original());
            for (const uint32_t entry : s_entries)
                if (!appendWord(rdram, ctx, runtime, vector, entry))
                    break;
            static uint32_t s_shown = 0;
            const uint32_t count = (gh2::load<uint32_t>(rdram, vector + 4u) - gh2::load<uint32_t>(rdram, vector)) / 4u;
            if (s_trace && count != s_shown)
                std::fprintf(stderr, "[gh1 songs] Quick Play: %u songs\n", count);
            s_shown = count;
            gh2::leave(ctx);
            return true;
        }

        // The Top Rockers (high scores) of each song are kept in a list made
        // once, when the Campaign is made (0x1374b0), from
        // CampaignData::GetAllSongs (0x13a8d8): the career's songs, without
        // the hidden ones, so their Top Rockers stayed empty and a score
        // there was never kept. For that one call the hidden songs go at
        // the end of what it returns: their lists get the same entries as
        // every song (Kendall 50000 and on), the save keeps them, and a save
        // without them loads as ever (songs are matched by name). Other
        // callers (the career's own counts) get the game's own answer.
        constexpr uint32_t kGetAllSongs = 0x13a8d8u; // vector<Symbol> CampaignData::GetAllSongs() const
        constexpr uint32_t kHighScoreDbCall = 0x13754cu;
        struct AllSongsTag;
        bool allSongs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 31) != kHighScoreDbCall)
                return false;
            const uint32_t out = GPR_U32(ctx, 4), self = GPR_U32(ctx, 5);
            runtime->callGuestFunction(rdram, ctx, kGetAllSongs, {out, self}, gh2::Override<AllSongsTag>::original());
            for (const uint32_t entry : s_entries)
            {
                uint32_t type = 0;
                const uint32_t song = node(rdram, entry, 0, &type);
                if (type != kTypeSymbol || !appendWord(rdram, ctx, runtime, out, song))
                    break;
            }
            if (s_trace)
                std::fprintf(stderr, "[gh1 songs] Top Rockers for %u songs\n",
                             (gh2::load<uint32_t>(rdram, out + 4u) - gh2::load<uint32_t>(rdram, out)) / 4u);
            SET_GPR_U32(ctx, 2, out);
            gh2::leave(ctx);
            return true;
        }

        // CampaignData::IsStoreSong(Symbol), 0x13b638 (found by its use: the
        // is_store_song message in Campaign::Handle, 0x1395e0, calls it at
        // 0x139b4c). Asked by that message only (the song intro's script,
        // ghui/game.dta: a store song shows its title and artist, any other
        // "as made famous by" its artist), the hidden songs answer yes:
        // they are originals, as the bonus songs are. Career code asking it
        // directly gets the game's own answer.
        constexpr uint32_t kIsStoreSong = 0x13b638u;
        constexpr uint32_t kIsStoreSongMessageReturn = 0x139b54u;
        struct StoreSongTag;
        bool isStoreSong(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (GPR_U32(ctx, 31) != kIsStoreSongMessageReturn)
                return false;
            const uint32_t song = GPR_U32(ctx, 5);
            for (const uint32_t entry : s_entries)
            {
                uint32_t type = 0;
                if (node(rdram, entry, 0, &type) == song)
                {
                    SET_GPR_U32(ctx, 2, 1u);
                    gh2::leave(ctx);
                    return true;
                }
            }
            return false;
        }
    }

    namespace
    {
        // GH1_TRACE=songs: the high scores' list of songs (0x1508c8, which
        // makes one entry per song of the song list) made after ours went in.
        struct HighScoresTag;
        void onHighScores(uint8_t *, R5900Context *, PS2Runtime *)
        {
            std::fprintf(stderr, "[gh1 songs] high score lists made %s the hidden songs went in\n",
                         s_entries.empty() ? "BEFORE" : "after");
        }
    }

    void installExtraSongs(PS2Runtime &runtime)
    {
        const char *trace = std::getenv("GH1_TRACE");
        s_trace = trace && std::strstr(trace, "songs");
        gh2::EntryHook<AddTag>::install(runtime, kDataVariable, &addSongs);
        gh2::Override<QuickplayTag>::install(runtime, kGetQuickplaySongs, &quickplaySongs);
        gh2::Override<StoreSongTag>::install(runtime, kIsStoreSong, &isStoreSong);
        gh2::Override<AllSongsTag>::install(runtime, kGetAllSongs, &allSongs);
        if (s_trace)
            gh2::EntryHook<HighScoresTag>::install(runtime, 0x1508c8u, &onHighScores);
    }
}

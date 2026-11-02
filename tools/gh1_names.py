#!/usr/bin/env python3
"""Name GH1's stripped retail ELF from GH2's retail ELF, with no debug build.

    tools/gh1_names.py <gh2_named.elf> <gh1_retail.elf> <out.symbols> <out.status>

gh2_named.elf is GH2 retail (SLUS-21447) symbolized with
config/gh2-retail.symbols. Both are retail discs a developer owns; only
function *names* cross over, never code: every function GH1 runs is GH1's own.

out.symbols is what symbolize.py reads (`address name`). out.status adds how
each name was found (exact, exact_by_order, fuzzy, score), because a name only
binds a function to a runtime handler when it was matched exactly
(see tools/gh1_stub_gate.py).
"""
import re
import sys

import disc
import match_symbols as ms


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    m = ms.Matcher(disc.load_elf(sys.argv[1]), disc.load_elf(sys.argv[2]))
    rounds = m.run()
    ms.report(m, rounds)
    # GH2's own unnamed functions are called func_<GH2 address>. Carried onto
    # GH1 that is a GH2 address on a GH1 function: no information, and it reads
    # like a GH1 address. Those GH1 functions stay unnamed instead.
    placeholder = re.compile(r"func_[0-9a-f]{8}$")
    for a, name, _ in m.funcs:
        if placeholder.match(name):
            m.results[a] = ("placeholder", None, 0)
    ms.write_symbols(m, sys.argv[3])
    seen = {}
    for a, name, _ in m.funcs:
        status, r, score = m.results[a]
        if r is not None and r not in seen:
            seen[r] = (name, status, score)
    with open(sys.argv[4], "w") as f:
        f.write("# gh1 address, name, how it was matched from GH2 retail, score\n")
        for r in sorted(seen):
            name, status, score = seen[r]
            f.write(f"{r:08x} {name} {status} {score if score is not None else '-'}\n")


if __name__ == "__main__":
    main()

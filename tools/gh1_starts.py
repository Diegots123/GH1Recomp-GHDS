#!/usr/bin/env python3
"""Function starts GH1 needs beyond match_symbols.retail_function_starts.

    tools/gh1_starts.py <gh1_retail.elf> <symbols-in> <symbols-out>

GH2's symbol table bounds nearly all of GH2's code, so the heuristic split only
fills gaps there. GH1's names cover far less, so more of its functions are
bounded by the heuristic alone, and one of its rules needs a return right
before a prologue. Between some GH1 functions the linker left filler that is
not nops (0x2b4050..0x2b4074 is one), so a callback after it, reached only
through a pointer stored in a struct, never became a function and the first
boot stopped there (JALR to 0x2b4078).

Added here: every address that code builds with lui/addiu, or that data holds,
which lands exactly on a stack-frame prologue (addiu $sp, $sp, -N). A pointer
to a prologue is a function pointer. The output is the input symbols plus
func_XXXXXXXX for each new start.
"""
import sys

import disc
import match_symbols as ms


JR_RA = 0x03E00008
NOP = 0


def is_prologue(w):
    return w >> 16 == 0x27BD and w & 0x8000


def is_epilogue(w):
    """addiu $sp, $sp, +N"""
    return w >> 16 == 0x27BD and not w & 0x8000


def is_unconditional_exit(w):
    """j, or jr $ra: nothing after its delay slot belongs to the same path."""
    return w >> 26 == 0x02 or w == JR_RA


def extra_starts(data):
    """Pointers to functions the split missed.

    Data pointers count only when they land on a prologue: jump tables in
    .rodata point at case labels, which follow unconditional branches.
    Addresses built in code (lui/addiu) are taken as function pointers when
    one of these also holds, each seen in GH1:
      - a prologue within two instructions (0x22cd78 schedules a mult first);
      - linker filler right before, `addiu $sp, $sp, +N` then nop
        (0x1ee5f8, 0x20cab0, 0x274e30: leaf callbacks with no frame);
      - a j or jr $ra and its delay slot right before (0x1001d8, a syscall
        wrapper after a tail call).
    Addresses built in code that pass none of these are labels inside a
    function (setjmp return points such as 0x102950), never starts."""
    secs, _, _ = ms.parse_elf(data)
    code = ms.code_words(data, secs)
    words = dict(code)
    addrs = set(words)
    vtable, drefs = ms.data_code_refs(data, secs, addrs)
    built = ms.code_built_refs(code, addrs)
    known = set(ms.retail_function_starts(data, secs, code))

    def word(a):
        return words.get(a, -1)

    def pointer_start(a):
        if is_prologue(word(a)) or is_prologue(word(a + 4)):
            return True
        if word(a - 4) == NOP and is_epilogue(word(a - 8)):
            return True
        return is_unconditional_exit(word(a - 8))

    out = {a for a in drefs | vtable if is_prologue(word(a))}
    out |= {a for a in built if pointer_start(a)}
    return sorted(a for a in out if a not in known)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    data = disc.load_elf(sys.argv[1])
    lines = open(sys.argv[2]).read().splitlines(True)
    named = {int(l.split()[0], 16) for l in lines if l.strip() and not l.startswith("#")}
    new = [a for a in extra_starts(data) if a not in named]
    entries = [l for l in lines if l.strip() and not l.startswith("#")]
    entries += [f"{a:08x} func_{a:08x}\n" for a in new]
    entries.sort(key=lambda l: int(l.split()[0], 16))
    with open(sys.argv[3], "w") as f:
        f.writelines([l for l in lines if l.startswith("#")] + entries)
    print(f"{len(new)} function starts added from code/data pointers to prologues", file=sys.stderr)


if __name__ == "__main__":
    main()

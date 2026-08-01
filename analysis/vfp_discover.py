#!/usr/bin/env python3
"""Discover VFP short-vector regions in libMassEffect.so.

The FPSCR LEN/STRIDE hazard: qemu-arm honours short-vector writes and
ARMv8 silently ignores (RAZ/WI), so the harness stays green while the console
computes only lane zero. A sibling port had 20 regions with 40 arithmetic
instructions, all in its audio mixer; this scan answers whether Mass Effect
Infiltrator carries any and emits the patch-list entries its expansion would
need.

Discovery only - it derives the list from the disassembly instead of checking
an existing one (that second half lives in a sibling port's vfp_coverage.py and
becomes relevant here once a patch list exists).

Usage:  python3 analysis/vfp_discover.py [path/to/all.dis]
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DIS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    HERE, "..", "..", "dis", "all.dis")

line_re = re.compile(r"^\s*([0-9a-f]+):\t([0-9a-f]{8}) \t(.*)$")
insns = []
with open(DIS) as f:
    for line in f:
        m = line_re.match(line)
        if m:
            insns.append((int(m.group(1), 16), int(m.group(2), 16),
                          m.group(3).strip()))

vmsr = [i for i, (a, w, t) in enumerate(insns) if t.startswith("vmsr")]
print(f"vmsr fpscr sites: {len(vmsr)}")

def preceding_len_value(idx):
    """ORR immediate into r0 shortly before the vmsr = LEN/STRIDE being set.

    objdump prints ARM immediates in decimal and then repeats them in hex in a
    trailing comment: "orr r0, r0, #196608\t@ 0x30000". The comment has to be
    cut off before choosing a base. A revision of this function that tested
    `"0x" in text` saw the *comment* and re-read the decimal 196608 as
    hexadecimal 0x196608, which decodes to LEN=1/STRIDE=1 instead of
    LEN=3/STRIDE=0 - a 2-lane stride-2 region that does not exist in the binary.
    That table shipped and detuned the game's audio mixer, so the operand is
    now parsed on its own, anchored at both ends.
    """
    for j in range(idx - 1, max(idx - 8, 0), -1):
        t = insns[j][2].split("@")[0]
        m = re.match(r"orr\s+r0, r0, #0x([0-9a-fA-F]+)\s*$", t)
        if m:
            return int(m.group(1), 16)
        m = re.match(r"orr\s+r0, r0, #(\d+)\s*$", t)
        if m:
            return int(m.group(1))
    return None

COND = r"(?:eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?"
VFP_ARITH = re.compile(r"^(vadd|vsub|vmul|vmla|vmls|vnmul|vnmla|vnmls|vdiv|"
                       r"vabs|vneg|vsqrt|vmov)" + COND + r"\.f(32|64)\b")

regions = []
i = 0
while i < len(vmsr):
    idx = vmsr[i]
    val = preceding_len_value(idx)
    if val and (val & 0x370000):
        if i + 1 < len(vmsr):
            regions.append((idx, vmsr[i + 1], val))
            i += 2
            continue
    i += 1

print(f"LEN-setting regions: {len(regions)}\n")

total = 0
entries = []
for open_i, close_i, lenval in regions:
    a_open, a_close = insns[open_i][0], insns[close_i][0]
    lanes = ((lenval >> 16) & 0x7) + 1
    stride = ((lenval >> 20) & 0x3) + 1
    body = insns[open_i + 1:close_i]
    arith = [(a, w, t) for (a, w, t) in body if VFP_ARITH.match(t)]
    total += len(arith)
    print(f"region +0x{a_open:06x}..+0x{a_close:06x}  LEN={lanes} "
          f"STRIDE={stride}  arith={len(arith)}")
    for a, w, t in arith:
        entries.append((a, w, lanes, stride, t))

print(f"\nTOTAL vector arithmetic instructions: {total}")
if entries:
    print("\nPatch-list entries (vfp_vector_patch.cpp format):")
    for a, w, lanes, stride, t in entries:
        print(f"    {{0x{a:08x}, 0x{w:08x}, {lanes}, {stride}}},   /* {t} */")
sys.exit(0)

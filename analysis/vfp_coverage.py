#!/usr/bin/env python3
"""Audit the VFP short-vector patch list against the binary.

src/vfp_vector_patch.cpp claims its entries are exactly the arithmetic
instructions inside the FPSCR LEN setup/reset pairs, expanded to the lane count
and stride the game itself asks for. Both halves of that claim are invisible to
the in-process self-test, and each fails silently in its own way:

  Coverage. An instruction left off the list runs as a full vector under
  qemu-arm, which still implements FPSCR LEN/STRIDE, so the harness stays green;
  on the R36S, where LEN reads as zero, the same instruction computes lane zero
  and discards the rest.

  Geometry. A listed instruction with the wrong lanes/stride patches cleanly and
  passes the self-test, because the self-test builds its reference FPSCR from
  the same patch.lanes/patch.stride it is checking - a wrong pair agrees with
  itself. This is not hypothetical: an earlier table said 2 and 6 lanes with a
  STRIDE=2 region, because vfp_discover.py had re-read objdump's decimal
  immediate as hex (196608 -> 0x196608). It shipped, and the 8-lane mixers
  computed 6 of every 8 samples - the constant 768/1024 non-zero ratio the
  device log showed, and audible as a detuned mixer. Nothing in the build caught
  it.

So the check runs off the binary in both directions: every instruction inside a
LEN region must be listed, nothing outside one may be, and each entry's
lanes/stride must equal what the region's own `orr` into FPSCR encodes.

Usage:  python3 analysis/vfp_coverage.py [path/to/all.dis]

The disassembly is produced with:
    arm-linux-gnueabihf-objdump -d libMassEffect.so > all.dis
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DIS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    HERE, "..", "..", "dis", "all.dis")
SRC = os.path.join(HERE, "..", "src", "vfp_vector_patch.cpp")

# ---- the patch list, read from the source so this cannot drift -------------
# {offset, expected_word, lanes, stride}
patched = {}
for m in re.finditer(r"\{0x([0-9a-f]{8}),\s*0x([0-9a-f]{8}),\s*(\d+),\s*(\d+)\}",
                     open(SRC).read()):
    patched[int(m.group(1), 16)] = (int(m.group(2), 16),
                                    int(m.group(3)), int(m.group(4)))
if not patched:
    print("ERROR: no patch entries parsed from vfp_vector_patch.cpp - the table "
          "format changed and this audit would pass vacuously")
    sys.exit(1)

# ---- parse the disassembly -------------------------------------------------
line_re = re.compile(r"^\s*([0-9a-f]+):\t([0-9a-f]{8}) \t(.*)$")
insns = []          # (addr, word, text)
by_addr = {}
with open(DIS) as f:
    for line in f:
        m = line_re.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        word = int(m.group(2), 16)
        text = m.group(3).strip()
        by_addr[addr] = len(insns)
        insns.append((addr, word, text))

vmsr = [i for i, (a, w, t) in enumerate(insns) if t.startswith("vmsr")]
print(f"vmsr fpscr sites: {len(vmsr)}")


# ---- classify each vmsr as setup or reset ----------------------------------
# Setup looks like:  vmrs r0,fpscr ; bic r0,r0,#0x370000 ; orr r0,r0,#LEN ;
#                    vmsr fpscr,r0
# Reset looks like:  vmrs r0,fpscr ; bic r0,r0,#0x370000 ; vmsr fpscr,r0
def preceding_len_value(idx):
    """The ORR immediate into r0 that this vmsr commits to FPSCR.

    objdump prints the immediate in decimal and repeats it in hex in a trailing
    comment ("orr r0, r0, #196608\t@ 0x30000"). The comment is cut off before
    choosing a base - reading the decimal as hex is precisely the bug this
    audit exists to catch, and it must not be reintroduced here.
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


regions = []
i = 0
while i < len(vmsr):
    idx = vmsr[i]
    val = preceding_len_value(idx)
    if val and (val & 0x370000):
        # find the matching close: the next vmsr
        if i + 1 < len(vmsr):
            regions.append((idx, vmsr[i + 1], val))
            i += 2
            continue
    i += 1

print(f"LEN-setting regions: {len(regions)}\n")

# Condition suffixes matter here: half the vector arithmetic in this binary is
# predicated (vaddvc.f32, vmlami.f32, ...), and a regex that only accepts the
# unconditional spelling silently reports those regions as empty.
COND = r"(?:eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?"
VFP_ARITH = re.compile(r"^(vadd|vsub|vmul|vmla|vmls|vnmul|vnmla|vnmls|vdiv|"
                       r"vabs|vneg|vsqrt|vmov)" + COND + r"\.f(32|64)\b")

total_covered = 0
total_uncovered = 0
uncovered_rows = []
geometry_rows = []

for open_i, close_i, lenval in regions:
    a_open = insns[open_i][0]
    a_close = insns[close_i][0]
    lanes = ((lenval >> 16) & 0x7) + 1
    stride = ((lenval >> 20) & 0x3) + 1
    body = insns[open_i + 1:close_i]
    arith = [(a, w, t) for (a, w, t) in body if VFP_ARITH.match(t)]
    cov = [x for x in arith if x[0] in patched]
    unc = [x for x in arith if x[0] not in patched]
    total_covered += len(cov)
    total_uncovered += len(unc)

    # The geometry check: what the table says vs what the game asks FPSCR for.
    bad_geom = [x for x in cov if patched[x[0]][1:] != (lanes, stride)]

    flags = ""
    if unc:
        flags += "   <-- UNCOVERED"
    if bad_geom:
        flags += "   <-- WRONG GEOMETRY"
    print(f"region +0x{a_open:06x}..+0x{a_close:06x}  fpscr=0x{lenval:x} "
          f"LEN={lanes} STRIDE={stride}  arith={len(arith)} "
          f"covered={len(cov)} uncovered={len(unc)}{flags}")
    for a, w, t in unc:
        uncovered_rows.append((a, w, t, lanes, stride))
    for a, w, t in bad_geom:
        geometry_rows.append((a, t, patched[a][1], patched[a][2], lanes, stride))

print()
print(f"TOTAL arithmetic inside LEN regions: {total_covered + total_uncovered}")
print(f"  covered by patch list : {total_covered}")
print(f"  NOT covered           : {total_uncovered}")
print(f"patch list entries      : {len(patched)}")

if uncovered_rows:
    print("\nUNCOVERED instructions (full vector on qemu, 1 lane on device):")
    for a, w, t, lanes, stride in uncovered_rows:
        print(f"    {{0x{a:08x}, 0x{w:08x}, {lanes}, {stride}}},   /* {t} */")

if geometry_rows:
    print("\nWRONG GEOMETRY (patches cleanly, computes the wrong lanes - this "
          "is the failure the self-test cannot see):")
    for a, t, have_l, have_s, want_l, want_s in geometry_rows:
        print(f"    +0x{a:08x}  table says lanes={have_l} stride={have_s}, "
              f"binary asks lanes={want_l} stride={want_s}   /* {t} */")

# ---- patch entries that fall outside any region ----------------------------
in_region = set()
for open_i, close_i, _ in regions:
    for a, w, t in insns[open_i + 1:close_i]:
        in_region.add(a)
stray = [a for a in patched if a not in in_region]
if stray:
    print("\nPatch entries NOT inside any LEN region (would corrupt scalar code):")
    for a in sorted(stray):
        j = by_addr.get(a)
        t = insns[j][2] if j is not None else "?"
        print(f"    +0x{a:08x}  {t}")

# ---- expected-word mismatches ---------------------------------------------
bad = []
for a, (expected, lanes, stride) in patched.items():
    j = by_addr.get(a)
    if j is None:
        bad.append((a, expected, None))
    elif insns[j][1] != expected:
        bad.append((a, expected, insns[j][1]))
if bad:
    print("\nExpected-word mismatches:")
    for a, e, f in bad:
        found = "not found" if f is None else f"{f:08x}"
        print(f"    +0x{a:08x} expected {e:08x} found {found}")
else:
    print("\nAll patch entries match the binary word-for-word.")

if not (uncovered_rows or geometry_rows or stray or bad):
    print("All patch entries carry the lanes/stride their region's FPSCR asks "
          "for.")

sys.exit(1 if (uncovered_rows or geometry_rows or stray or bad) else 0)

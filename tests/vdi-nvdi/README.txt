VDI NVDI colour arithmetic checks
=================================

make -C tests/vdi-nvdi check builds the arithmetic of EmuTOS's NVDI colour
calls (vdi/vdi_nvdimath.c) straight out of the EmuTOS tree with the host gcc,
-m32 (32-bit long, as on the target) and ASan/UBSan, and runs test_nvdimath.
EMUTOS= names the tree (default ../../../emutos-vdi-fonts). Requires gcc with
32-bit multilib and the 32-bit ASan/UBSan runtimes.

The file is pure arithmetic and EmuTOS is built with 16-bit ints, so the
check first fails if vdi_nvdimath.c or its header contains a plain int
(or unsigned int): sizes, products and distances are LONG/ULONG there.

test_nvdimath checks:
- the VDI scale 0-1000 against the 16-bit one 0-65535 for every value from
  0 to 1000: converting there and back gives the value again, the scale
  rises, values outside 0-1000 are clamped, 500 is 0x8000;
- the Falcon 16-bit pixel rrrrrggggg0bbbbb: pack(unpack(v)) == v for all
  32768 values with bit 5 clear; for every 16-bit value of each component,
  alone, unpack(pack(c)) is within 0x842 of c and bits outside the component
  stay clear; the top five bits are cut, not rounded; pack(ffff,0,0) is
  0xf800;
- the nearest entry of a table of colours (squared distance in the 0-1000
  VDI units, out of range components clamped): on the default ST, STE
  (4 bits a component), TT and Falcon palettes, cut out of vdi/vdi_col.c so
  the real tables are used, each entry is its own nearest, or the lowest of
  the equal entries; of equal distances the lowest entry wins, also over
  several tables scanned in turn.
Temporary files go to obj/ and are removed by make clean.

vr_clip_rects (clip_pair, bring_along, muldiv32 of the same file) is checked
against a reference in 64-bit arithmetic, which only the test uses:
- the five vectors of the guest test NVDIMISC, and refused inputs (upside
  down rectangles, a side of 2^31 pixels or more);
- every 16-bit pair of sizes 1-24, offsets -8 to 32, against six clips, by
  destination and by source;
- 32-bit sizes across 2^16 and 2^31 and starts up to +-0x7fffffff, with
  clips at the ends of the range;
- muldiv32 for 10^6 pseudo-random triples, and its edges (a quotient over
  0x7fffffff is capped, operands out of range give 0).

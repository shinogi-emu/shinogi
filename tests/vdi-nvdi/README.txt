VDI NVDI colour arithmetic and raster transfer checks
=====================================================

make -C tests/vdi-nvdi check builds the arithmetic of EmuTOS's NVDI colour
calls (vdi/vdi_nvdimath.c) and the pixel conversion of vr_transfer_bits
(vdi/vdi_xfercore.c) straight out of the EmuTOS tree with the host gcc,
-m32 (32-bit long, as on the target) and ASan/UBSan, and runs test_nvdimath
and test_xfer.
EMUTOS= names the tree (default ../../../emutos-vdi-fonts). Requires gcc with
32-bit multilib and the 32-bit ASan/UBSan runtimes.

The files are pure (no hardware, no VDI globals) and EmuTOS is built with
16-bit ints, so the check first fails if vdi_nvdimath.c, vdi_xfercore.c or
their headers contain a plain int (or unsigned int): sizes, products and
distances are LONG/ULONG there.

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

test_xfer checks the conversion of vr_transfer_bits against direct formulas
on 64-bit integers, not against the rational stepping the code runs:
- the unpackers of every source format (15 and 16 bits, each in both byte
  orders, 24 bits in both orders, 32 bits; every 16-bit value for the 16-bit
  ones) and the 5-6-5 packer (the green keeps its sixth bit; a 5-6-5 pixel
  survives unpack and pack);
- the source kind of each pixel format, and of the formats that are none
  (1, 8 and 32 bits are never reversed, planar formats are no source);
- every size 1-40 by 1-40 of source width and destination width, with
  heights paired with them in two ways, for all nine source kinds (1 bit,
  8-bit indexed, 15, 16, 24 and 32 bits, the reversible ones both ways),
  onto 16-bit words and onto 1, 2, 4 and 8 planes: the pixel of every
  position is the nearest source pixel (x * sw / dw), and for a reduction of
  a direct source onto 16 bits the four-sample filter of fVDI's
  tests/transfer.c (centred, eight fractional bits); sources with a byte
  offset and a line longer than the pixels, a 1-bit source with a bit
  offset, transparent 1-bit sources (only the 1 bits arrive);
- windows of the destination (the sampling phase does not move when the
  part drawn starts inside it), for sizes 1-40 in all combinations of eight
  sizes;
- extreme coordinates: a source or destination of 65536 pixels, skips at the
  ends of 16-bit coordinates, in both directions of scaling;
- the planar packer alone (planes 1, 2, 4, 8, any start column, any count,
  with and without a mask): only the pixels named change;
- the lazy table of an 8-bit source onto planes (each index is asked for
  once and none that is not there), an enlarged row that repeats;
- refusals: no address, bits that do not say the format, a line too short,
  an odd address or line for 16- and 32-bit pixels, an origin or size outside
  the 16-bit coordinates, more than 2^31 - 1 bytes, destinations of 3 or 24
  planes, rectangles outside the bitmap or upside down, memory that overlaps.

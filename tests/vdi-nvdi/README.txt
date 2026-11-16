VDI NVDI colour arithmetic and raster transfer checks
=====================================================

make -C tests/vdi-nvdi check builds the arithmetic of EmuTOS's NVDI colour
calls (vdi/vdi_nvdimath.c) and the pixel conversion of vr_transfer_bits
(vdi/vdi_xfercore.c) straight out of the EmuTOS tree with the host gcc,
-m32 (32-bit long, as on the target) and ASan/UBSan, and runs test_nvdimath,
test_xfer, test_dither and test_bezier.
EMUTOS= names the tree (default ../../emutos, the submodule). Requires gcc with
32-bit multilib and the 32-bit ASan/UBSan runtimes.

The files are pure (no hardware, no VDI globals) and EmuTOS is built with
16-bit ints, so the check first fails if vdi_nvdimath.c, vdi_xfercore.c,
vdi_bezcore.c or their headers contain a plain int (or unsigned int):
sizes, products and distances are LONG/ULONG there.

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

test_dither checks the index path of vdi_xfercore.c (8-bit indexed
destinations, and dithering, mode 128) against references independent of
it, as fVDI's tests/indexed.c has them:
- the inverse colour maps (v_create_itab, and the tables the VDI makes for
  its own palettes): every cell at 3, 4 and 5 bits a component, for random
  palettes of 1 to 256 colours with and without equal colours, against a
  full search at the centre of the cell (squared distance at 8 bits, the
  first of equals); the lookup of random colours; a grey ramp of cell
  centres finds itself; the work memory is exactly XFER_ITAB_WORK(count);
- the remapping of 8-bit sources: the same index if it has the colour,
  else the first index with it, else the inverse map; the source colours;
  the flags for every colour found exactly and for every index staying;
- rows of the index path for 8-bit sources and every direct source kind
  (15, 16, 24 bits in both orders, 32 bits), with and without diffusion,
  enlarged, reduced and 1:1 (sizes 1-31 paired, and 61x113, 197x7 onto
  320x9 and 160x50), onto palettes of 2 to 256 colours: each index equals
  fVDI's reference (nearest or four-sample filtered colour, Floyd-Steinberg
  over two wide error rows, an 8-bit source whose colours are all in the
  destination keeping its indices and not dithered); a window of the
  rectangle equals the same part of the whole (the diffusion runs through
  the parts not drawn); guard bytes around each row stay;
- a mid grey dithered onto black and white is about half white, no index
  beyond the palette;
- the memory of an 8-bit indexed destination: its size, and the refusals
  (a line too short, another pixel format, other bits, no address);
- xfer_remap given counts over 256 (257, 1000, 32767): it takes 256, into
  tables of exactly 256 entries.

test_bezier checks vdi/vdi_bezcore.c, the points of the Bezier curves
(v_bez, v_bez_fill), against fVDI's own code (engine/bezier.c: bez_depth,
do_bez4 and the loops of calc_bez, copied into the test with the two
differences the VDI documents: a curve needs its four points, flags only
as far as given):
- 6000 random inputs of 1-63 points, random curve and move flags, as many
  flags as points less one or fewer, each depth scale, closed and open,
  three clipping rectangles, coordinates small and across the whole 16-bit
  range: the count of points needed, the moves, whether anything is a curve
  or a move, the points made and the polylines they start, as fVDI's;
- the count is an upper bound of the points made;
- with less room than needed (one short, half, none) the making refuses
  and writes nothing past the room; the points, the flags and the output
  are allocated to their exact size, so ASan sees any read or write beyond;
- no points; three points with a curve's flag (not a curve).

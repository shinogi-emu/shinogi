VDI NVDI colour arithmetic and raster transfer checks
=====================================================

make -C tests/vdi-nvdi check builds the arithmetic of EmuTOS's NVDI colour
calls (vdi/vdi_nvdimath.c), the pixel conversion of vr_transfer_bits
(vdi/vdi_xfercore.c), the points of its Bezier curves (vdi/vdi_bezcore.c)
the blending of its anti-aliased text (vdi/vdi_textaa.c) and the blending of
images with alpha (vdi/vdi_alpha.c) straight out of the EmuTOS tree with the
host gcc, -m32 (32-bit long, as on the target) and ASan/UBSan, and runs
test_nvdimath, test_xfer, test_xfer_alpha, test_dither, test_bezier,
test_textaa and test_alpha.  vdi_xfercore.c is built twice: as the ROMs
have it, and with CONF_WITH_VDI_ALPHA=1 (the blending of images with
alpha).  test_alpha uses the second; test_xfer_alpha is test_xfer built
against it, and must print the same counts as test_xfer (make compares
their output), so the switch changes nothing the old modes do.
EMUTOS= names the tree (default ../../emutos, the submodule). Requires gcc with
32-bit multilib and the 32-bit ASan/UBSan runtimes.

The files are pure (no hardware, no VDI globals) and EmuTOS is built with
16-bit ints, so the check first fails if vdi_nvdimath.c, vdi_xfercore.c,
vdi_bezcore.c, vdi_textaa.c, vdi_alpha.c or their headers contain a plain int (or unsigned int):
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

test_textaa checks vdi/vdi_textaa.c, the blending of anti-aliased outline
text into the 16-bit pixels (rrrrrggggg0bbbbb, the only direct-colour
format the VDI draws in), against the rule the file documents, worked out
in floating point: coverage c is step (c + 4) / 8 of 32, and a component
goes from d to d + (f - d) * step / 32, rounded half up:
- the step of every coverage 0-255: 0-3 leave a pixel, 252-255 are the
  text colour, 128 is half way, the steps rise;
- every pair of 5-bit components at every step 0-32, in each of the three
  positions with the other components random: as the rule, between the two
  colours, moving towards the text colour as the step rises, the other
  components kept; 2,000,000 random pixels and steps; bit 5 of a blend is
  clear; step 0 is the pixel (bit 5 too), step 32 the text colour;
- areas of 1-19 x 1-7 pixels, of the basic colours in pairs and of random
  colours up to 80 x 40, in replace and transparent mode, over a plain
  background, two alternating colours, stripes of the text colour and
  random pixels, with coverage of every kind (plenty of 0, 255 and their
  neighbours 3, 4, 251, 252) and mostly blank as text is, starting at
  every alignment and in rows with padding: each pixel as the rule (from
  the background colour in replace mode, from the pixel in transparent
  mode), and a frame of known pixels around the area unchanged; areas of
  no width or height;
- windows cut at every side, every start column, out of a 23 x 11 area
  (as the VDI starts a clipped bitmap at sx,sy), dense and sparse, both
  modes: the same pixels as the whole there, nothing else;
- the turns by 90, 180 and 270 degrees of images 1-17 x 1-13 and 300 x 40
  with padded rows: each pixel where one-bit text is turned, the padding
  of the result untouched; four quarter turns, and a quarter and three
  quarters, give the image back.

test_alpha checks vdi/vdi_alpha.c, the blending of 32-bit pixels with
straight alpha (PX_ARGB32, PX_RGBA32) into 5-6-5 pixels, against the
arithmetic below worked out in 64-bit integers in the test itself (it
shares no code with the file) and against alpha_vectors.h:
- the checksum of the vectors, recomputed first;
- the weight Wc' of every 16-bit weight; kc for every alpha under the six
  weights of the vectors, each channel; 1,000,000 random alphas and weights;
- every field value, every 8-bit component and every kc of 0-256, in each
  field of the pixel (red and blue at five bits, green at six), the other
  two with kc 0: the field as the reference, the other fields unchanged
  (about 8.4 million mixes);
- the 4096 pixels of the vectors through the mix and through a row of one
  pixel, in both orders of bytes (alpha 0 gives d, the others the expected
  pixel); 2,000,000 random pixels and weight triples, both orders;
  alpha 255 at full weight is xfer_pack565()'s pixel;
- full weight, every alpha 128-255, every green value 0-63 against every
  component 0-255 (a shortcut to the opaque pixel may not take alpha 252 or lower, where
  a field and the component's top bits 43 or more apart differ; 253 and
  254 give the opaque pixel in 5-6-5, so they cannot be told from it);
- rows of 1-80 pixels starting at each of four word positions in a buffer
  with a frame, both orders of bytes, alphas all 0, all 255, mixed
  (plenty of 0, 1, 127, 128, 254, 255) and a ramp, at full weight and
  under each weight of the vectors: each pixel as the reference, the
  pixels of alpha 0 and the frame untouched.

It also checks T_BLEND through the raster transfer core (vdi_xfercore.c,
xfer_begin and xfer_row16 with job.blend), against the nearest source
pixel sx = x * sw / dw, sy = y * sh / dh worked out directly and the
reference above:
- the kinds: PX_ARGB32 and PX_RGBA32 and nothing near them (never
  reversed, 0x04122020 is none), 32 bits, the alpha of each kind (255 for
  those without one), their colour for 100,000 random pixels;
  xfer_blend_ok() for every kind against every destination (direct sources
  onto 16-bit words only), and every refusal of the vectors decided as
  vdi_xfer.c decides it;
- xfer_src_size(): an odd address or line, a line shorter than 4 x w, bits
  other than 32 refused; sources of 1-9 x 1-5 pixels in memory of exactly
  width x h bytes and destinations of exactly their size, drawn whole 1:1,
  enlarged and reduced (ASan sees any byte read past them);
- every size 1-40 by 1-40 of source and destination width with two
  heights each, for ARGB32, RGBA32, 15, 16 and 24 bits (both orders) and
  xRGB (alpha 255), under full weight and the six weights of the vectors in
  turn, over random backgrounds with a frame: each pixel as the reference
  (nearest, never filtered), the frame untouched; enlarged rows are blended
  each into their own background, never copied from the row above;
- windows of the destination starting inside it, for sizes 1-40 in all
  combinations of eight: the same pixels as the whole run there, nothing
  else;
- sources and destinations of 65536 pixels, skips at the ends of 16-bit
  coordinates;
- alpha 255 at full weight is mode 32: the same job without blending, for
  every kind, wherever mode 32 does not filter (1:1 and enlargements);
- modes 0 and 32 draw ARGB32 and RGBA32 as xRGB of the same colours (the
  alpha not read) onto 16 bits (filtered when reduced), onto planes and as
  8-bit indices with and without dithering;
- every transfer of the vectors, in both orders of bytes, drawn onto its
  background clipped as it says: the CRC-32 of the destination is
  crc565, and the ramp equals alpha_ramp565 in full.

Alpha vectors
-------------

alpha_vectors.h holds the reference vectors for blending images with
straight alpha onto 16-bit pixels (vr_transfer_bits, mode T_BLEND = 64, the
sources PX_ARGB32 0x03122020 and PX_RGBA32 0x03022020).  It is written by
gen_alpha_vectors.c, whose reference functions are its own (64-bit integers,
written from the formulas, sharing no code with any VDI), and it is the file
every implementation is checked against.  The same file is committed in the
fVDI tests.  It holds: six weight triples; kc for every alpha under each
weight; 4096 pixel cases (565 and 555, with the expected results); four
source images (icon 16x12, a disc centred in its frame with pixel (x,y)
centred at (x,y), ramp 7x5, dot 1x1, column 1x9); three 32x24 backgrounds in
both layouts (background 2 in 555 has bit 15 set at random in about half of
its pixels, so transfers onto it pin the write rule: a written pixel clears
it, an untouched one keeps it); 44 transfers (scaling, offsets, clipping,
weights) with the CRC-32 of the resulting destination; the 565 result of the
ramp in full; 18 calls that must be refused.  The layout and the meaning of
each field are in the comment at the top of the file.

The arithmetic, per channel c of r, g, b, all integers (>> only on
non-negative values), with the weight Wc (0-65535), the source alpha A
(0-255), a destination field of n bits (5 or 6) holding d and the 8-bit
source component C:

    Wc' = Wc + (Wc >> 15)
    wa  = A + (A >> 7)
    kc  = (wa * Wc' + 32768) >> 16            0-256
    s   = C >> (8 - n)
    out = (d * 256 + (s - d) * kc + 128) >> 8

A = 0 leaves the pixel alone; any other pixel is written with 0 outside the
three fields.  Before it writes anything the generator checks: kc is 0 for
alpha 0, 256 for alpha 255 at weight 0xffff, at most 256 and rising with the
alpha (for all 65536 weights); the numerator is 128-16256; out is d at kc 0,
s at kc 256 and between d and s (for both field widths, every d, C and kc).

ALPHA_VECTORS_SUM is the zlib CRC-32 (reflected 0xEDB88320, initial value and
final xor 0xffffffff) of every array in declaration order, each scalar element,
whatever its width, appended as 4 bytes big-endian (a negative value sign-
extended), the fields of a struct in order; for alpha_src its w, h and then the
pixels it points to, never the pointer.  A consumer recomputes it and compares
it before anything else.

make -C tests/vdi-nvdi vectors rewrites alpha_vectors.h (the output is the same
on every host: a fixed seed, no clock).  make vectors-check regenerates it into
obj/ and compares it with the committed file (it is part of make check); with
FVDI_TESTS=<directory> it compares the copy there too.

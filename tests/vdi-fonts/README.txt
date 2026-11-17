VDI outline font engine checks
==============================

make -C tests/vdi-fonts check builds EmuTOS's outline font engine
(vdi/vdi_ft2.c, vdi/vdi_ftmap.c), its memory allocator (vdi/vdi_ftmem.c) and
its FreeType subset straight out of the EmuTOS tree with the host gcc, -m32
(32-bit long, as on the target) and ASan/UBSan, and runs four programs:
test_ftmem checks the allocator on its own, test_vdi_fonts the engine with
host_ftos.c in place of the GEMDOS services of vdi/vdi_ftos.c,
test_vdi_fonts_ftmem the engine again with every block from the allocator,
whose arenas come from malloc(), and test_stack the engine on a stack of the
size of EmuTOS's font stack; check-config compares FreeType's images under
EmuTOS's FreeType settings with those under FreeType's own. EMUTOS= names
the tree (default ../../emutos, the submodule); SANS=, SECOND= and MONO= name
the fonts (default Liberation Sans, DejaVu Sans and DejaVu Sans Mono under
/usr/share/fonts/truetype), CFF= a CFF OpenType font (default Nimbus Sans
from the URW base 35 fonts) and CONFIG_FONTS= more fonts for check-config.
Requires gcc with 32-bit multilib and the 32-bit ASan/UBSan runtimes.
Temporary files go to a fresh folder under $TMPDIR (or /tmp) and are
removed afterwards.

test_stack runs every engine call on a stack of VDI_FT_STACK_SIZE bytes
(vdi/vdi_ft2.h) made with makecontext(), above a 256-byte canary, with
memory refused within VDI_FT_STACK_GUARD bytes of the bottom, as EmuTOS's
allocator (vdi/vdi_ftmem.c) refuses it on the font stack: test_stack with
host_ftos.c's malloc() stand-in making the check, test_stack_ftmem with
vdi/vdi_ftmem.c making it, both asking host_ftos.c's outline_stack_low().
Its fonts are made by mkfonts
from CFF= and SECOND=: a copy of the CFF font whose glyphs, but 'A' and
'acute', are the implied seac form of endchar (an accented A, which FreeType
interprets one level down, about 16 KB of stack a level), and copies of the
TrueType font whose glyphs 1-60, 200-1399 and 1-1200 form chains of
composite glyphs. First, without the guard on a 2 MB stack, the seac glyph
must need over 8 KB more stack than a plain one, and the deepest glyph of
the long chain more than the whole font stack. Then, on the font stack,
every character up to U+FFFF of each font is drawn at 16 and 72 pixels and
compared with FreeType's own image, drawn on the main stack: the CFF font,
the seac font and the short chain draw every character as FreeType does,
with no allocation refused; the long chain draws as FreeType does what it
draws, refuses only composite glyphs, and the guard refuses some
allocations; the chain through 'A' is not registered (its 'A' cannot be
loaded). A copy of the TrueType font whose prep program loops for ever,
which FreeType stops at the instruction limit and then refuses to hint at
that size, must draw "Abg8" as FreeType draws it unhinted. The canary must
stay untouched, no block may be left after vdi_ft_term, and the CFF font
must draw again at the end.

check-config builds render_crc twice, once against EmuTOS's FreeType
configuration and once with FreeType's own render pool (16384 bytes,
EmuTOS's freetype/config/ftoption.h has 8192) and bytecode instruction
limit (a million, EmuTOS's Makefile sets 50000). Both checksum every glyph
image of the fonts at 8 to 300 pixels, monochrome and grey; the two outputs
must be the same.

Liberation Sans is registered and sized at 12 pt, 16 pixels. "Atari" renders
as wide as the sum of its advances and as high as top + bottom, matches its
extent, and draws its measured glyphs again without loading them (one
allocation for the whole string). Each Atari code 0x80-0xFF maps to the
Unicode character of the NVDI/fVDI table (Atari to BICS to Unicode) and
renders pixel for pixel as FreeType's own image of that glyph; codes the
font lacks render as its .notdef glyph. Unicode and BICS codes, a code drawn
under two maps on the same font, codes above 0x7FFF and an SPDCHAR.MAP that
remaps 'A' are checked the same way.

A truncated copy of the font (its first 4 KiB), garbage, an empty file and a
missing file are refused without leaking, and so are copies that FreeType
still opens: cut at 60000 bytes (glyphs and names gone, which used to give a
face named " " that drew nothing) and at 300000 bytes (no names). The font
still registers afterwards. With every allocation failing, starting, registering, sizing
and rendering (fresh and cached glyphs, codes above 255, a new effect)
return errors and leak nothing, and the same font draws once memory is back.
Then each allocation in turn is made to fail through a whole session; every
run must leak nothing, recover, and keep no reference to the font file: once
another font has pushed out its open faces and the engine has dropped its
idle file buffers (as it does when a file it needs is gone), sizing the font
again must read its file again. Sizes whose cell or width would not fit in
32000 pixels are refused and leave the file just as releasable.

Effects 0-31 and skew render with extents equal to their bitmaps; bold and
italic widen, light lightens, underline adds full rows. A width set apart
from the height narrows the text but not the cell. Outlines, vqt_xfntinfo
and vqt_fontheader data, monospace extents, unique ids across three fonts,
and 50 sizes of one face (open faces stay bounded, the file is read once,
the first size still draws) are checked next.

Coverage, for anti-aliased text (vdi_ft_render with VDI_FT_COVERAGE), is
checked last: each printable ASCII character, and seven strings ("ffffjjj///"
and "AVAWAY" among them, so that glyphs overlap), render as FreeType's own
grey images laid at the pen positions, united where they overlap as the
engine documents (a + b - a * b / 255); the box, origin and advance are
those of the one-plane string, the rows padded to whole longs. For effects
0-31 the box and extent are those of one-plane text, a pixel is fully
covered (but by the half-pixel lines of outlined text), light text never
passes half (and is half of plain text glyph by glyph), underlined text has
as many full rows as the underline is thick, bold is wider and darker,
italic and skew wider. After a coverage string, a measured string is drawn
with one allocation (the measuring rendered its coverage), and its one-plane
image is still made when asked for. Without memory a coverage string is
refused and nothing leaks; failing at each of 29 allocations of a new glyph
keeps at most the cached glyph. 448 glyphs of 100 pixels (two instances,
three million bytes of strings) keep at most 256 KB of images plus
overhead (the stand-in counts the bytes of live blocks; the vdi_ftmem.c
build does not tell, and skips this), and the first glyph, whose image went,
draws again the same.

test_ftmem gets the allocator's system blocks from malloc(), cut to size
and only 2-byte aligned as GEMDOS blocks can be. With outline_stack_low()
saying the font stack is nearly used up, nothing is given, and a block that
cannot grow is left as it was. It checks every block is
8-byte aligned, from size 0 up and across the 16 KB line between arena and
own blocks; that 16 KB blocks come from arenas and larger ones each from a
system block of their own; that arenas are taken as needed and given back
when empty, one being kept; that every one of the 5040 orders of freeing
seven blocks merges their arena whole (seen as the arena going back to the
system); realloc across sizes, in place when it fits, and leaving the old
block in use when it fails; requests with no memory at all, with no new
system blocks (served from the arenas), and with no block as big as an
arena (small blocks get their own); and 400000 random allocations, frees
and reallocs against overlap, overwriting and the memory taken from the
system.

Host stand-in leaks show as counted blocks left after vdi_ft_term, the
sanitizers stop at the first memory error, and LeakSanitizer reports
anything else left at exit. The test prints one FAIL line per failed check
and ends with the number of checks and failures.

VDI outline font engine checks
==============================

make -C tests/vdi-fonts check builds EmuTOS's outline font engine
(vdi/vdi_ft2.c, vdi/vdi_ftmap.c) and its FreeType subset straight out of the
EmuTOS tree with the host gcc, -m32 (32-bit long, as on the target) and
ASan/UBSan, links it with host_ftos.c in place of the GEMDOS services of
vdi/vdi_ftos.c, and runs it. EMUTOS= names the tree (default
../../../emutos-vdi-fonts); SANS=, SECOND= and MONO= name the fonts (default
Liberation Sans, DejaVu Sans and DejaVu Sans Mono under
/usr/share/fonts/truetype). Requires gcc with 32-bit multilib and the 32-bit
ASan/UBSan runtimes. Temporary files go to a fresh folder under $TMPDIR
(or /tmp) and are removed afterwards.

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
missing file are refused without leaking, and the font still registers
afterwards. With every allocation failing, starting, registering, sizing
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
the first size still draws) are checked last.

Host stand-in leaks show as counted blocks left after vdi_ft_term, the
sanitizers stop at the first memory error, and LeakSanitizer reports
anything else left at exit. The test prints one FAIL line per failed check
and ends with the number of checks and failures.

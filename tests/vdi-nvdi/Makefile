# Host checks for the arithmetic of the NVDI colour calls of EmuTOS's VDI
# (vdi/vdi_nvdimath.c) and for the pixel conversion of vr_transfer_bits
# (vdi/vdi_xfercore.c), with its inverse colour tables and dithering.
#
# The file is built straight out of the EmuTOS tree with the host gcc, -m32
# (32-bit long, as on the target) and ASan/UBSan.  It is pure arithmetic,
# so it needs no stubs.  The default palettes are cut out of vdi/vdi_col.c
# so that the test checks the tables the VDI really starts with.  The file
# must not use a plain int (EmuTOS is built with 16-bit ints): that is
# checked too.  See README.txt.
EMUTOS ?= ../../emutos

CC  ?= gcc
OBJ = obj

ARCH = -m32
SAN  = -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
# only portab.h is wanted from EmuTOS's include/
CFLAGS = $(ARCH) $(SAN) -O1 -g -Wall -Wextra -Werror -std=gnu99 -I$(EMUTOS)/vdi -idirafter $(EMUTOS)/include

PURE = $(EMUTOS)/vdi/vdi_nvdimath.c $(EMUTOS)/vdi/vdi_nvdimath.h \
       $(EMUTOS)/vdi/vdi_xfercore.c $(EMUTOS)/vdi/vdi_xfercore.h \
       $(EMUTOS)/vdi/vdi_bezcore.c $(EMUTOS)/vdi/vdi_bezcore.h

check: noint $(OBJ)/test_nvdimath $(OBJ)/test_xfer $(OBJ)/test_dither $(OBJ)/test_bezier
	$(OBJ)/test_nvdimath
	$(OBJ)/test_xfer
	$(OBJ)/test_dither
	$(OBJ)/test_bezier

# the pure files contain no plain int
noint:
	@if grep -nE '(^|[^A-Za-z0-9_])(unsigned[[:space:]]+)?int([^A-Za-z0-9_]|$$)' $(PURE); then \
	    echo "plain int in a pure file"; exit 1; fi

$(OBJ)/test_nvdimath: $(OBJ)/test_nvdimath.o $(OBJ)/vdi_nvdimath.o
	$(CC) $(ARCH) $(SAN) -o $@ $^

$(OBJ)/test_xfer: $(OBJ)/test_xfer.o $(OBJ)/vdi_xfercore.o
	$(CC) $(ARCH) $(SAN) -o $@ $^

$(OBJ)/test_dither: $(OBJ)/test_dither.o $(OBJ)/vdi_xfercore.o
	$(CC) $(ARCH) $(SAN) -o $@ $^

$(OBJ)/test_dither.o: test_dither.c $(EMUTOS)/vdi/vdi_xfercore.h $(EMUTOS)/vdi/vdi_nvdiext.h | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/test_bezier: $(OBJ)/test_bezier.o $(OBJ)/vdi_bezcore.o
	$(CC) $(ARCH) $(SAN) -o $@ $^

$(OBJ)/test_bezier.o: test_bezier.c $(EMUTOS)/vdi/vdi_bezcore.h | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/vdi_bezcore.o: $(EMUTOS)/vdi/vdi_bezcore.c $(EMUTOS)/vdi/vdi_bezcore.h | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/vdi_xfercore.o: $(EMUTOS)/vdi/vdi_xfercore.c $(EMUTOS)/vdi/vdi_xfercore.h $(EMUTOS)/vdi/vdi_nvdiext.h | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/test_xfer.o: test_xfer.c $(EMUTOS)/vdi/vdi_xfercore.h | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/vdi_nvdimath.o: $(EMUTOS)/vdi/vdi_nvdimath.c $(EMUTOS)/vdi/vdi_nvdimath.h | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/test_nvdimath.o: test_nvdimath.c $(OBJ)/palettes.h $(EMUTOS)/vdi/vdi_nvdimath.h | $(OBJ)
	$(CC) $(CFLAGS) -I$(OBJ) -c $< -o $@

# the default palettes of vdi_col.c: st_palette, tt_palette1, tt_palette2, videl_palette1 and 2
$(OBJ)/palettes.h: $(EMUTOS)/vdi/vdi_col.c | $(OBJ)
	awk '/^static const WORD (st_palette|tt_palette1|tt_palette2|videl_palette1|videl_palette2)\[/ {on=1} on {print} on && /^};/ {on=0}' $< > $@

$(OBJ):
	mkdir -p $@

clean:
	rm -rf $(OBJ)

.PHONY: check noint clean

# The copy paths of the VDI to the screen (vdi_raster.c, vdi_xfer.c) stay on the
# screen, whatever the clip flag says.  vdi_raster.c is linked as it is but for
# the line that says an m68k assembler blit exists (the host has none) and the
# reading of an MFDB's address out of CONTRL, two words, high first as on the 68k; with
# the blitter and the unrolled 16-bit copies, which are 68k code, off.
# vdi_alpha.c and the T_BLEND checks are built if the tree has them.
CLAMPDEF = -DCONF_WITH_BLITTER=0 -DCONF_WITH_VDI_FASTDRAW=0
ifneq ($(wildcard $(EMUTOS)/vdi/vdi_alpha.c),)
CLAMPDEF += -DCONF_WITH_VDI_ALPHA=1
CLAMPALPHA = $(OBJ)/clamp_vdi_alpha.o
endif
# EmuTOS's include/ after the host's own, as for the other checks: it has a string.h of its own
CLAMPCF = $(ARCH) $(SAN) -O1 -g -Wall -Wno-unused-function -std=gnu99 -I$(EMUTOS)/vdi -idirafter $(EMUTOS)/include \
          -I$(EMUTOS)/bios $(CLAMPDEF)

check: check-clamp check-clamp-rom

CLAMPCFROM = $(ARCH) $(SAN) -O1 -g -Wall -Wno-unused-function -std=gnu99 -I$(EMUTOS)/vdi -idirafter $(EMUTOS)/include \
             -I$(EMUTOS)/bios -DCONF_WITH_BLITTER=0 -DCONF_WITH_VDI_FASTDRAW=0 -DCONF_WITH_VDI_NVDI_EXT=0

check-clamp: $(OBJ)/test_clamp
	$(OBJ)/test_clamp

$(OBJ)/test_clamp: $(OBJ)/test_clamp.o $(OBJ)/clamp_vdi_raster.o $(OBJ)/clamp_vdi_xfer.o \
                   $(OBJ)/clamp_vdi_xfercore.o $(CLAMPALPHA)
	$(CC) $(ARCH) $(SAN) -o $@ $^

$(OBJ)/clamp_shim.h: | $(OBJ)
	printf '#include "emutos.h"\n#include "asm.h"\n#undef rorw1\n#define rorw1(x) x=(x>>1)|(x<<15)\n' > $@

$(OBJ)/clamp_vdi_raster.c: $(EMUTOS)/vdi/vdi_raster.c | $(OBJ)
	sed -e 's/define ASM_BLIT_IS_AVAILABLE   1 /define ASM_BLIT_IS_AVAILABLE   0 /' \
	    -e 's/\*(MFDB \*\*)&CONTRL\[\([79]\)\]/(MFDB *)(((ULONG)(UWORD)CONTRL[\1] << 16) | (UWORD)CONTRL[\1 + 1])/' $< > $@

$(OBJ)/clamp_vdi_raster.o: $(OBJ)/clamp_vdi_raster.c $(OBJ)/clamp_shim.h
	$(CC) $(CLAMPCF) -include $(OBJ)/clamp_shim.h -c $< -o $@

$(OBJ)/clamp_vdi_xfer.o: $(EMUTOS)/vdi/vdi_xfer.c | $(OBJ)
	$(CC) $(CLAMPCF) -c $< -o $@

$(OBJ)/clamp_vdi_xfercore.o: $(EMUTOS)/vdi/vdi_xfercore.c | $(OBJ)
	$(CC) $(CLAMPCF) -c $< -o $@

$(OBJ)/clamp_vdi_alpha.o: $(EMUTOS)/vdi/vdi_alpha.c | $(OBJ)
	$(CC) $(CLAMPCF) -c $< -o $@

$(OBJ)/test_clamp.o: test_clamp.c | $(OBJ)
	$(CC) $(CLAMPCF) -c $< -o $@

.PHONY: check-clamp

# The same without the NVDI extensions (the 192 and 256 KB targets and the like): the
# destination is bounded, a source and a form are trusted.  vdi_raster.c alone.
check-clamp-rom: $(OBJ)/test_clamp_rom
	$(OBJ)/test_clamp_rom

$(OBJ)/test_clamp_rom: $(OBJ)/test_clamp_rom.o $(OBJ)/clamp_rom_vdi_raster.o
	$(CC) $(ARCH) $(SAN) -o $@ $^

$(OBJ)/test_clamp_rom.o: test_clamp.c | $(OBJ)
	$(CC) $(CLAMPCFROM) -c $< -o $@

$(OBJ)/clamp_rom_vdi_raster.o: $(OBJ)/clamp_vdi_raster.c $(OBJ)/clamp_shim.h
	$(CC) $(CLAMPCFROM) -include $(OBJ)/clamp_shim.h -c $< -o $@

.PHONY: check-clamp-rom

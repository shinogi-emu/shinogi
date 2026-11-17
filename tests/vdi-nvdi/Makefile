# Host checks for the arithmetic of the NVDI colour calls of EmuTOS's VDI
# (vdi/vdi_nvdimath.c), for the pixel conversion of vr_transfer_bits
# (vdi/vdi_xfercore.c), with its inverse colour tables and dithering, for
# the points of its Bezier curves (vdi/vdi_bezcore.c) and for the blending
# of its anti-aliased text (vdi/vdi_textaa.c).
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
       $(EMUTOS)/vdi/vdi_bezcore.c $(EMUTOS)/vdi/vdi_bezcore.h \
       $(EMUTOS)/vdi/vdi_textaa.c $(EMUTOS)/vdi/vdi_textaa.h

check: noint $(OBJ)/test_nvdimath $(OBJ)/test_xfer $(OBJ)/test_dither $(OBJ)/test_bezier \
       $(OBJ)/test_textaa
	$(OBJ)/test_nvdimath
	$(OBJ)/test_xfer
	$(OBJ)/test_dither
	$(OBJ)/test_bezier
	$(OBJ)/test_textaa

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

$(OBJ)/test_textaa: $(OBJ)/test_textaa.o $(OBJ)/vdi_textaa.o
	$(CC) $(ARCH) $(SAN) -o $@ $^ -lm

$(OBJ)/test_textaa.o: test_textaa.c $(EMUTOS)/vdi/vdi_textaa.h | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/vdi_textaa.o: $(EMUTOS)/vdi/vdi_textaa.c $(EMUTOS)/vdi/vdi_textaa.h | $(OBJ)
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

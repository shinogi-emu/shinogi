# Host checks for the outline font engine of EmuTOS's VDI (vdi/vdi_ft2.c)
# and its memory allocator (vdi/vdi_ftmem.c).
#
# The engine, its character set tables and the FreeType subset are built
# straight out of the EmuTOS tree with the host gcc, -m32 (32-bit long, as
# on the target) and ASan/UBSan, against host_ftos.c instead of GEMDOS.
# See README.txt.
EMUTOS ?= ../../../emutos-vdi-fonts
FONTS  ?= /usr/share/fonts/truetype
SANS   ?= $(FONTS)/liberation/LiberationSans-Regular.ttf
SECOND ?= $(FONTS)/dejavu/DejaVuSans.ttf
MONO   ?= $(FONTS)/dejavu/DejaVuSansMono.ttf

CC  ?= gcc
OBJ = obj

ARCH = -m32
SAN  = -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
FT   = $(EMUTOS)/freetype

FT_CFLAGS = $(ARCH) $(SAN) -O1 -g -I. -I$(FT)/config -I$(FT)/include \
            -DFT2_BUILD_LIBRARY \
            '-DFT_CONFIG_OPTIONS_H=<ftoption.h>' \
            '-DFT_CONFIG_MODULES_H=<ftmodule.h>' \
            '-DFT_CONFIG_STANDARD_LIBRARY_H=<host_ftstdlib.h>'
# EmuTOS's include/ is searched after the host headers: only portab.h is
# wanted from it, not its own string.h
CFLAGS    = $(FT_CFLAGS) -I$(EMUTOS)/vdi -idirafter $(EMUTOS)/include -Wall -Wextra -Werror

# the same FreeType sources as the EmuTOS Makefile (FT_SOURCES)
FT_SOURCES = base/ftbase.c base/ftinit.c base/ftbbox.c base/ftbitmap.c \
             base/ftdebug.c base/ftglyph.c base/ftmm.c base/ftstroke.c \
             base/ftsynth.c \
             sfnt/sfnt.c truetype/truetype.c cff/cff.c psaux/psaux.c \
             pshinter/pshinter.c psnames/psnames.c smooth/smooth.c \
             raster/raster.c autofit/autofit.c
FT_OBJ = $(foreach f,$(FT_SOURCES),$(OBJ)/ft_$(subst /,_,$(basename $(f))).o)

ENGINE_OBJ = $(OBJ)/vdi_ft2.o $(OBJ)/vdi_ftmap.o
TEST_OBJ   = $(OBJ)/test_vdi_fonts.o $(OBJ)/host_ftos.o

# test_ftmem checks the allocator of vdi/vdi_ftmem.c on its own;
# test_vdi_fonts_ftmem runs the engine checks again with every engine
# allocation going through that allocator
check: $(OBJ)/test_ftmem $(OBJ)/test_vdi_fonts $(OBJ)/test_vdi_fonts_ftmem
	$(OBJ)/test_ftmem
	$(OBJ)/test_vdi_fonts $(SANS) $(SECOND) $(MONO)
	$(OBJ)/test_vdi_fonts_ftmem $(SANS) $(SECOND) $(MONO)

$(OBJ)/test_ftmem: $(OBJ)/test_ftmem.o $(OBJ)/vdi_ftmem.o
	$(CC) $(ARCH) $(SAN) -o $@ $^

$(OBJ)/test_vdi_fonts: $(TEST_OBJ) $(ENGINE_OBJ) $(FT_OBJ)
	$(CC) $(ARCH) $(SAN) -o $@ $^

$(OBJ)/test_vdi_fonts_ftmem: $(OBJ)/test_vdi_fonts.o $(OBJ)/host_ftos_ftmem.o \
                             $(OBJ)/vdi_ftmem.o $(ENGINE_OBJ) $(FT_OBJ)
	$(CC) $(ARCH) $(SAN) -o $@ $^

$(OBJ)/host_ftos_ftmem.o: host_ftos.c host_ftos.h host_ftstdlib.h $(wildcard $(EMUTOS)/vdi/vdi_ft*.h) | $(OBJ)
	$(CC) $(CFLAGS) -DHOST_FTMEM -c $< -o $@

$(OBJ):
	mkdir -p $@

define FT_OBJ_RULE
$(OBJ)/ft_$(subst /,_,$(basename $(1))).o: $(FT)/src/$(1) host_ftstdlib.h | $(OBJ)
	$$(CC) $$(FT_CFLAGS) -c $$< -o $$@
endef
$(foreach f,$(FT_SOURCES),$(eval $(call FT_OBJ_RULE,$(f))))

$(OBJ)/vdi_%.o: $(EMUTOS)/vdi/vdi_%.c $(wildcard $(EMUTOS)/vdi/vdi_ft*.h) host_ftstdlib.h | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/%.o: %.c host_ftos.h host_ftstdlib.h $(wildcard $(EMUTOS)/vdi/vdi_ft*.h) | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJ)

.PHONY: check clean

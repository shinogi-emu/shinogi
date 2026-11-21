# Host checks for the outline font engine of EmuTOS's VDI (vdi/vdi_ft2.c)
# and its memory allocator (vdi/vdi_ftmem.c), on a stack of the size
# EmuTOS gives the engine, and of EmuTOS's FreeType settings.
#
# The engine, its character set tables and the FreeType subset are built
# straight out of the EmuTOS tree with the host gcc, -m32 (32-bit long, as
# on the target) and ASan/UBSan, against host_ftos.c instead of GEMDOS.
# See README.txt.
EMUTOS ?= ../../emutos
FONTS  ?= /usr/share/fonts/truetype
SANS   ?= $(FONTS)/liberation/LiberationSans-Regular.ttf
SECOND ?= $(FONTS)/dejavu/DejaVuSans.ttf
MONO   ?= $(FONTS)/dejavu/DejaVuSansMono.ttf
# a font that also carries the Atari codes at U+F000 + code
CLASSIC ?= ../../docs/tt68070-coordination/freemint-amp-20261112/recipe/desktop/fonts/CLASSIC/REGULAR.TTF
CFF    ?= /usr/share/fonts/opentype/urw-base35/NimbusSans-Regular.otf
# more fonts for the comparison of FreeType's settings (check-config)
CONFIG_FONTS ?=

CC  ?= gcc
OBJ = obj

ARCH = -m32
SAN  = -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
FT   = $(EMUTOS)/freetype

# the instruction limit is the one of EmuTOS's Makefile (its FT_CFLAGS)
FT_CFLAGS = $(ARCH) $(SAN) -O1 -g -I. -I$(FT)/config -I$(FT)/include \
            -DFT2_BUILD_LIBRARY -DTT_CONFIG_OPTION_MAX_RUNNABLE_OPCODES=50000L \
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

# the fonts of test_stack: every glyph of the CFF font built of two
# others (seac); glyphs 1-60 of the second font a chain of composite
# glyphs, glyphs 200-1399 one, and glyphs 1-1200 (through 'A') one; and
# the second font with a prep program that never ends
STACK_FONTS = $(OBJ)/SEAC.OTF $(OBJ)/CHAIN60.TTF $(OBJ)/CHAIN1200.TTF $(OBJ)/CHAINA.TTF \
              $(OBJ)/LOOP.TTF

# test_ftmem checks the allocator of vdi/vdi_ftmem.c on its own;
# test_vdi_fonts_ftmem runs the engine checks again with every engine
# allocation going through that allocator; test_stack runs the engine on
# a stack of the size of EmuTOS's font stack, over fonts that mkfonts
# makes, and test_stack_ftmem again with the allocator, whose own guard
# refuses memory near the bottom of the stack; check-config compares
# FreeType's images under EmuTOS's settings with those under FreeType's own
check: $(OBJ)/test_ftmem $(OBJ)/test_vdi_fonts $(OBJ)/test_vdi_fonts_ftmem \
       $(OBJ)/test_stack $(OBJ)/test_stack_ftmem $(STACK_FONTS) check-config
	$(OBJ)/test_ftmem
	$(OBJ)/test_vdi_fonts $(SANS) $(SECOND) $(MONO) $(CLASSIC)
	$(OBJ)/test_vdi_fonts_ftmem $(SANS) $(SECOND) $(MONO) $(CLASSIC)
	$(OBJ)/test_stack $(CFF) $(STACK_FONTS)
	$(OBJ)/test_stack_ftmem $(CFF) $(STACK_FONTS)

$(OBJ)/SEAC.OTF: $(OBJ)/mkfonts
	$(OBJ)/mkfonts seac $(CFF) $@
$(OBJ)/CHAIN60.TTF: $(OBJ)/mkfonts
	$(OBJ)/mkfonts chain $(SECOND) $@ 60 1
$(OBJ)/CHAIN1200.TTF: $(OBJ)/mkfonts
	$(OBJ)/mkfonts chain $(SECOND) $@ 1200 200
$(OBJ)/CHAINA.TTF: $(OBJ)/mkfonts
	$(OBJ)/mkfonts chain $(SECOND) $@ 1200 1
$(OBJ)/LOOP.TTF: $(OBJ)/mkfonts
	$(OBJ)/mkfonts loop $(SECOND) $@

$(OBJ)/mkfonts: $(OBJ)/mkfonts.o $(OBJ)/host_ftos.o $(FT_OBJ)
	$(CC) $(ARCH) $(SAN) -o $@ $^

$(OBJ)/test_stack: $(OBJ)/test_stack.o $(OBJ)/host_ftos.o $(ENGINE_OBJ) $(FT_OBJ)
	$(CC) $(ARCH) $(SAN) -o $@ $^

$(OBJ)/test_stack_ftmem: $(OBJ)/test_stack.o $(OBJ)/host_ftos_ftmem.o $(OBJ)/vdi_ftmem.o \
                         $(ENGINE_OBJ) $(FT_OBJ)
	$(CC) $(ARCH) $(SAN) -o $@ $^

# FreeType with its own values of what EmuTOS changes: the render pool of
# the rasterizers (freetype/config/ftoption.h) and the bytecode instruction
# limit (FT_CFLAGS); only these three modules use them
STOCK_SOURCES = raster/raster.c smooth/smooth.c truetype/truetype.c
STOCK_CFLAGS  = -I$(OBJ)/stock $(filter-out -DTT_CONFIG_OPTION_MAX_RUNNABLE_OPCODES=%,$(FT_CFLAGS))
STOCK_OBJ     = $(foreach f,$(STOCK_SOURCES),$(OBJ)/stock/ft_$(subst /,_,$(basename $(f))).o)
STOCK_SHARED  = $(filter-out $(foreach f,$(STOCK_SOURCES),$(OBJ)/ft_$(subst /,_,$(basename $(f))).o),$(FT_OBJ))

$(OBJ)/stock/ftoption.h: $(FT)/config/ftoption.h | $(OBJ)
	mkdir -p $(OBJ)/stock
	sed 's/^#define FT_RENDER_POOL_SIZE .*/#define FT_RENDER_POOL_SIZE  16384L/' $< > $@
	grep -q 'FT_RENDER_POOL_SIZE  16384L' $@

define STOCK_OBJ_RULE
$(OBJ)/stock/ft_$(subst /,_,$(basename $(1))).o: $(FT)/src/$(1) $(OBJ)/stock/ftoption.h host_ftstdlib.h
	$$(CC) $$(STOCK_CFLAGS) -c $$< -o $$@
endef
$(foreach f,$(STOCK_SOURCES),$(eval $(call STOCK_OBJ_RULE,$(f))))

$(OBJ)/render_crc: $(OBJ)/render_crc.o $(OBJ)/host_ftos.o $(FT_OBJ)
	$(CC) $(ARCH) $(SAN) -o $@ $^

$(OBJ)/render_crc_stock: $(OBJ)/render_crc.o $(OBJ)/host_ftos.o $(STOCK_SHARED) $(STOCK_OBJ)
	$(CC) $(ARCH) $(SAN) -o $@ $^

check-config: $(OBJ)/render_crc $(OBJ)/render_crc_stock
	$(OBJ)/render_crc $(SANS) $(SECOND) $(MONO) $(CFF) $(CONFIG_FONTS) > $(OBJ)/crc-emutos.txt
	$(OBJ)/render_crc_stock $(SANS) $(SECOND) $(MONO) $(CFF) $(CONFIG_FONTS) > $(OBJ)/crc-stock.txt
	! grep -q 'cannot open' $(OBJ)/crc-emutos.txt
	cmp $(OBJ)/crc-emutos.txt $(OBJ)/crc-stock.txt
	@echo "config: $$(wc -l < $(OBJ)/crc-emutos.txt) font sizes and modes drawn the same with EmuTOS's FreeType settings"

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
$(OBJ)/ft_$(subst /,_,$(basename $(1))).o: $(FT)/src/$(1) $(FT)/config/ftoption.h host_ftstdlib.h | $(OBJ)
	$$(CC) $$(FT_CFLAGS) -c $$< -o $$@
endef
$(foreach f,$(FT_SOURCES),$(eval $(call FT_OBJ_RULE,$(f))))

$(OBJ)/vdi_%.o: $(EMUTOS)/vdi/vdi_%.c $(wildcard $(EMUTOS)/vdi/vdi_ft*.h) host_ftstdlib.h | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/%.o: %.c host_ftos.h host_ftstdlib.h $(wildcard $(EMUTOS)/vdi/vdi_ft*.h) | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJ)

.PHONY: check check-config clean

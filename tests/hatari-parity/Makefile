# Differential test: our host->Atari 8.3 mapping against Hatari's real
# Str_Filename_Host2Atari(), both compiled from their own source trees
# and linked into one native binary. No part of Hatari is copied here.
#
# HATARI_BUILD is needed only for the generated config.h that Hatari's
# own headers include; the build tree is not otherwise used.
EMUTOS      ?= $(HOME)/git/emutos
HATARI      ?= $(HOME)/git/Hatari
HATARI_BUILD ?= $(HATARI)/build

CFLAGS = -Wall -Wextra -O1
OUR_CFLAGS  = $(CFLAGS) -DMACHINE_QEMU_VIRT -I$(EMUTOS)/bdos -I$(EMUTOS)/include -I.
THEIR_CFLAGS = $(CFLAGS) -I$(HATARI)/src/includes -I$(HATARI_BUILD) -I$(HATARI)/src

OBJS = test_hatari_parity.o hostfs_name.o str.o hatari_config_stub.o

test: test_hatari_parity
	./test_hatari_parity

test_hatari_parity: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

# ours
test_hatari_parity.o: test_hatari_parity.c
	$(CC) $(CFLAGS) -c -o $@ $<

hostfs_name.o: $(EMUTOS)/bdos/hostfs_name.c
	$(CC) $(OUR_CFLAGS) -c -o $@ $<

# theirs, verbatim
str.o: $(HATARI)/src/str.c
	$(CC) $(THEIR_CFLAGS) -c -o $@ $<

hatari_config_stub.o: hatari_config_stub.c
	$(CC) $(THEIR_CFLAGS) -c -o $@ $<

clean:
	rm -f test_hatari_parity $(OBJS)

.PHONY: test clean

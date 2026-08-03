# Builds the pure name-mapping unit natively, straight out of the
# EmuTOS tree, so the Hatari rules can be checked without booting.
EMUTOS ?= $(HOME)/git/emutos

CFLAGS = -Wall -Wextra -O1 -DMACHINE_QEMU_VIRT -I$(EMUTOS)/bdos -I.

test: test_hostfs_name
	./test_hostfs_name

test_hostfs_name: test_hostfs_name.c $(EMUTOS)/bdos/hostfs_name.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f test_hostfs_name

.PHONY: test clean

# Builds the pure epoch->GEMDOS-date/time unit natively, straight out
# of the EmuTOS tree, so the packing/clamping rules can be checked
# without booting. Mirrors tests/hostfs-name/Makefile.
EMUTOS ?= $(HOME)/git/emutos

CFLAGS = -Wall -Wextra -O1 -DMACHINE_QEMU_VIRT \
         -I$(EMUTOS)/bios -I$(EMUTOS)/include -I.

test: test_epochtime
	./test_epochtime

test_epochtime: test_epochtime.c $(EMUTOS)/bios/epochtime.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f test_epochtime

.PHONY: test clean

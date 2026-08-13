CROSS ?= m68k-atari-mintelf-
QEMU ?= $(HOME)/git/atari-docs/qemu-m68k/build-gui/qemu-system-m68k

.PHONY: all check clean

all: m68060.elf

m68060.o: m68060.S
	$(CROSS)gcc -c -nostdlib -m68020-60 -o $@ $<

m68060.elf: m68060.o kernel.ld
	$(CROSS)ld -m m68kelf -T kernel.ld -o $@ m68060.o

check: m68060.elf
	@output=`$(QEMU) -M virt -cpu m68060 -m 16 -kernel $< \
		-display none -serial stdio -no-reboot`; \
	printf '%s\n' "$$output"; \
	test "$$output" = "M68060 TEST PASS"

clean:
	rm -f m68060.o m68060.elf

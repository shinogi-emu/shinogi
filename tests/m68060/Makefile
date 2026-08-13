CROSS ?= m68k-atari-mintelf-
QEMU ?= $(HOME)/git/atari-docs/qemu-m68k/build-gui/qemu-system-m68k

.PHONY: all check bench clean

all: m68060.elf fpu-bench.elf fpu-conv-bench.elf

m68060.o: m68060.S
	$(CROSS)gcc -c -nostdlib -m68020-60 -o $@ $<

m68060.elf: m68060.o kernel.ld
	$(CROSS)ld -m m68kelf -T kernel.ld -o $@ m68060.o

fpu-bench.o: fpu-bench.S
	$(CROSS)gcc -c -nostdlib -m68020-60 -o $@ $<

fpu-bench.elf: fpu-bench.o kernel.ld
	$(CROSS)ld -m m68kelf -T kernel.ld -o $@ fpu-bench.o

fpu-conv-bench.o: fpu-conv-bench.S
	$(CROSS)gcc -c -nostdlib -m68020-60 -o $@ $<

fpu-conv-bench.elf: fpu-conv-bench.o kernel.ld
	$(CROSS)ld -m m68kelf -T kernel.ld -o $@ fpu-conv-bench.o

check: m68060.elf
	@output=`$(QEMU) -M virt -cpu m68060 -m 16 -kernel $< \
		-display none -serial stdio -no-reboot`; \
	printf '%s\n' "$$output"; \
	test "$$output" = "M68060 TEST PASS"

bench: fpu-bench.elf fpu-conv-bench.elf
	@for image in $^; do \
		echo "$$image"; \
		for run in 1 2 3 4 5; do \
			/usr/bin/time -f '%e' $(QEMU) -M virt -cpu m68060 -m 16 \
				-kernel $$image -display none -serial none -no-reboot; \
		done; \
	done

clean:
	rm -f m68060.o m68060.elf fpu-bench.o fpu-bench.elf \
		fpu-conv-bench.o fpu-conv-bench.elf

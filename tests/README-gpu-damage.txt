Framebuffer and responsiveness checks
====================================

Compile tests/test-gpu-damage.c natively with ASan/UBSan. It compares every
output pixel and the preserved 16-bit shadow against independent full-frame
reference results across all supported dimensions, random edits, idle frames
and boundary pixels. GPU_DAMAGE_HEADER may select a candidate guest header.

The optimized comparison must also run as actual 68060 code. Use the Atari
ELF compiler with -m68020-60 -mshort -nostdlib -ffreestanding -fno-builtin -O2
-DGUEST -Wl,--oformat=elf32-m68k,-e,_start,-Ttext,0x1000 and -lgcc. Run that
ELF on the project QEMU with -M virt -cpu m68060 -m 64 -display none
-monitor none -serial stdio. Success is P and exit0. A failure includes the
source line and width/height/damage bounds/changed pixel coordinates.

Do not substitute a generic Linux compiler with -mshort: its size_t can
be 16 bits and silently wraps offsets in large framebuffers. The Atari
toolchain uses 32-bit size_t, matching the actual firmware ABI.

tools/check-responsiveness.py runs actual old/new firmware with isolated
copies of the release guest tree. It verifies a full desktop boot, injects
mouse movement through QMP, checks the real cursor pixels in screenshots,
and records latency, flush rectangles/rate, idle CPU and boot timing. It
requires the old 10 Hz behavior as a failure control and responsive candidate
behavior without an idle CPU regression. This is a headless guest-path
measurement; Windows SDL, screen refresh and compositor latency are outside
that measurement. It is not an ARAnyM throughput comparison.

The guest filesystem regression scaffolding is now opt-in through QEMU
-append shinogi.selftest=1. The golden and host-write harnesses request this
explicitly; release launchers leave it disabled. Use disposable test drives.

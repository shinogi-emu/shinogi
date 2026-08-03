/*
 * Stand-in for the EmuTOS build configuration header.
 *
 * bdos/hostfs_name.c includes "config.h" so that its MACHINE_QEMU_VIRT
 * guard is real rather than silently compiling to nothing. The EmuTOS
 * include directory cannot simply be put on this test's include path:
 * it also holds its own string.h, which would then shadow the host's
 * for the test program itself. MACHINE_QEMU_VIRT arrives via -D, so
 * nothing else is needed here.
 */

#!/bin/sh
#
# Launch shinogi in a window.
#
# The eventual Phase 9 launcher grows from this: the guest artifact is
# host-independent, so all that ever changes per platform is which QEMU
# binary is invoked and how it is bundled.
#
# Usage: tools/run-shinogi.sh [elf] [display]
#
set -eu

ELF="${1:-$HOME/git/emutos/emutos-virt.elf}"
#
# gtk, not sdl. With an absolute pointing device SDL grabs the pointer as
# soon as it moves inside a focused window, and only releases it again
# when the pointer touches a window edge (ui/sdl2.c handle_mousemotion).
# Where that grab stops motion being delivered -- X11/XWayland under a
# remote session, at least -- the edge can never be reached and the guest
# pointer is dead until the window loses focus.
#
# GTK never grabs while the device is absolute, and ungrabs if a device
# becomes absolute (ui/gtk.c:695, ui/gtk.c:1081), so a tablet behaves the
# way it is supposed to. macOS/cocoa keeps the pointer associated in
# absolute mode and is fine too.
DISP="${2:-gtk}"
LOG="${TMPDIR:-/tmp}/shinogi-serial.log"

if [ ! -f "$ELF" ]; then
    echo "no guest image at $ELF" >&2
    echo "build it with:" >&2
    echo "  cd ~/git/emutos && make ELF=1 TOOLCHAIN_PREFIX=m68k-atari-mintelf- \\" >&2
    echo "       QEMU_VIRT_DEFS=-DENABLE_KDEBUG qemu-virt" >&2
    exit 1
fi

echo "shinogi: $ELF"
echo "display: $DISP    serial log: $LOG"

# -serial file: rather than stdio, so the window is the only thing the
# user has to look at. -d guest_errors costs nothing and turns a silent
# virtio mistake into a line in the log.
exec qemu-system-m68k \
    -M virt \
    -m 128 \
    -kernel "$ELF" \
    -device virtio-gpu-device \
    -device virtio-keyboard-device \
    -device virtio-tablet-device \
    -display "$DISP" \
    -serial "file:$LOG" \
    -d guest_errors -D "$LOG.err"

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
# sdl by default: one backend on every platform, which keeps the three
# bundles behaving the same way. Verified working on Windows.
#
# BUT SDL IS UNUSABLE ON SOME LINUX SESSIONS. With an absolute pointing
# device SDL grabs the pointer as soon as it moves inside a *focused*
# window, and only releases it when the pointer touches a window edge
# (ui/sdl2.c handle_mousemotion). Where the grab also stops motion being
# delivered -- x11/XWayland under GNOME Remote Login, measured here --
# the edge can never be reached, so the grab never lifts and the guest
# pointer is frozen until the window loses focus.
#
# If the pointer will not move, pass gtk:
#
#     tools/run-shinogi.sh "" gtk
#
# GTK never grabs while the device is absolute and ungrabs when one
# appears (ui/gtk.c:695, ui/gtk.c:1081); it also has a menubar and can
# scale the window, neither of which SDL offers. tools/sdl-grab-probe.c
# tells you whether a given machine is affected.
DISP="${2:-sdl}"
SCALE="${SHINOGI_SCALE:-1.5}"
LOG="${TMPDIR:-/tmp}/shinogi-serial.log"

# The host folder exposed as the guest's drive C: over virtio-9p. Override
# with SHINOGI_HOSTFS to point the guest at a different directory.
HOSTFS="${SHINOGI_HOSTFS:-$HOME/shinogi-drive-c}"
mkdir -p "$HOSTFS"

#
# Window sizing, gtk only.
#
# zoom-to-fit defaults to *on* for virtio-gpu, because QEMU assumes a
# guest that can be told about window resizes will follow along. Ours
# cannot -- the resolution is fixed at build time -- so the default
# leaves GTK picking its own window size and scaling 1280x720 down into
# it. Turning it off sizes the window to the guest instead.
#
# scale then enlarges that, keeping GEM text and icons readable rather
# than giving them more pixels to shrink into. It arrived in QEMU 10.1,
# so probe for it rather than hard-failing on an older build: asking for
# a deliberately invalid value reports the type on a QEMU that has the
# option and "unexpected" on one that does not, and either way the
# option parser rejects it long before a window is created.
#
if [ "${DISP%%,*}" = gtk ] && [ "$DISP" = gtk ]; then
    DISP="gtk,zoom-to-fit=off"
    if qemu-system-m68k -M virt -m 16 -display "gtk,scale=bogus" 2>&1 \
       | grep -q "for 'scale'"; then
        DISP="$DISP,scale=$SCALE"
    else
        echo "note: this QEMU predates -display gtk,scale; window will" >&2
        echo "      be 1280x720. QEMU 10.1 or newer scales it up." >&2
    fi
fi

if [ ! -f "$ELF" ]; then
    echo "no guest image at $ELF" >&2
    echo "build it with:" >&2
    echo "  cd ~/git/emutos && make ELF=1 TOOLCHAIN_PREFIX=m68k-atari-mintelf- \\" >&2
    echo "       QEMU_VIRT_DEFS=-DENABLE_KDEBUG qemu-virt" >&2
    exit 1
fi

echo "shinogi: $ELF"
echo "display: $DISP    serial log: $LOG"
echo "drive C: $HOSTFS"

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
    -fsdev "local,id=hostfs,path=$HOSTFS,security_model=mapped-xattr" \
    -device virtio-9p-device,fsdev=hostfs,mount_tag=shinogi \
    -display "$DISP" \
    -serial "file:$LOG" \
    -d guest_errors -D "$LOG.err"

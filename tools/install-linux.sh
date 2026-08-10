#!/bin/sh
#
# Install shinogi for the current user, so it appears in the desktop's
# application menu and can be launched without a terminal.
#
#   tools/install-linux.sh
#
# Everything lands under ~/.local, so no root is needed and uninstalling
# is deleting four files.
#
# QEMU: the PATCHED build, not the system one.  The distribution
# qemu-system-m68k aborts about six seconds in and the guest never reaches
# MINT.PRG -- patches/0003 and 0006 are what make FreeMiNT boot here at
# all.  This install used to take whatever was on PATH and produced a
# machine that came up to a bare EmuTOS desktop and looked like an old
# build.  SHINOGI_QEMU overrides the search.
#
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(cat "$ROOT/VERSION")
ELF="${SHINOGI_ELF:-$HOME/git/emutos/emutos-virt.elf}"

BIN="$HOME/.local/bin"
SHARE="$HOME/.local/share"
LIBDIR="$SHARE/shinogi"

[ -f "$ELF" ] || { echo "no guest image at $ELF - build it first" >&2; exit 2; }

QEMU="${SHINOGI_QEMU:-$HOME/git/atari-docs/qemu-m68k/build-vvfat/qemu-system-m68k}"
if [ ! -x "$QEMU" ]; then
    echo "no patched QEMU at $QEMU" >&2
    echo "" >&2
    echo "The system qemu-system-m68k will NOT do: it aborts a few seconds" >&2
    echo "into the boot and the guest never reaches MINT.PRG, which looks" >&2
    echo "like an old build coming up to a bare EmuTOS desktop." >&2
    echo "Build the patched tree, or point SHINOGI_QEMU at it." >&2
    exit 2
fi

mkdir -p "$BIN" "$LIBDIR" "$SHARE/applications" \
         "$SHARE/icons/hicolor/256x256/apps" \
         "$SHARE/icons/hicolor/128x128/apps" \
         "$SHARE/icons/hicolor/64x64/apps" \
         "$SHARE/icons/hicolor/48x48/apps"

# The launcher looks for the guest image beside itself, so both live in
# the private lib dir and only a symlink goes on PATH.
cc "$ROOT/tools/shinogi-launcher.c" \
   -DSHINOGI_VERSION="\"$VERSION\"" \
   -o "$LIBDIR/shinogi" -O2 -Wall
cp "$ELF" "$LIBDIR/emutos-virt.elf"

# The launcher prefers qemu/bin/qemu-system-m68k beside itself and only
# falls back to PATH, so putting the patched build here is what stops the
# fallback ever being reached. A symlink, not a copy: this is a local
# install pointing at a local build, and a stale copy of a QEMU that is
# still being patched would be worse than none.
mkdir -p "$LIBDIR/qemu/bin"
ln -sf "$QEMU" "$LIBDIR/qemu/bin/qemu-system-m68k"
ln -sf "$LIBDIR/shinogi" "$BIN/shinogi"

cp "$ROOT/tools/linux/shinogi.png"     "$SHARE/icons/hicolor/256x256/apps/shinogi.png"
cp "$ROOT/tools/linux/shinogi-128.png" "$SHARE/icons/hicolor/128x128/apps/shinogi.png"
cp "$ROOT/tools/linux/shinogi-64.png"  "$SHARE/icons/hicolor/64x64/apps/shinogi.png"
cp "$ROOT/tools/linux/shinogi-48.png"  "$SHARE/icons/hicolor/48x48/apps/shinogi.png"

sed "s|@BINDIR@|$LIBDIR|g" "$ROOT/tools/linux/shinogi.desktop.in" \
    > "$SHARE/applications/shinogi.desktop"

update-desktop-database "$SHARE/applications" 2>/dev/null || true
gtk-update-icon-cache -f -t "$SHARE/icons/hicolor" 2>/dev/null || true

echo "installed shinogi $VERSION"
echo "  launcher : $LIBDIR/shinogi  (symlinked as $BIN/shinogi)"
echo "  desktop  : $SHARE/applications/shinogi.desktop"
echo "  drive C  : $HOME/shinogi-drive-c"
echo
echo "It should now appear in the application menu. If it does not, log"
echo "out and back in - some desktops only rescan at session start."

#!/bin/sh
#
# Install shinogi for the current user, so it appears in the desktop's
# application menu and can be launched without a terminal.
#
#   tools/install-linux.sh
#
# Everything lands under ~/.local, so no root is needed and uninstalling
# is deleting four files. Uses the system QEMU.
#
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(cat "$ROOT/VERSION")
ELF="${SHINOGI_ELF:-$HOME/git/emutos/emutos-virt.elf}"

BIN="$HOME/.local/bin"
SHARE="$HOME/.local/share"
LIBDIR="$SHARE/shinogi"

[ -f "$ELF" ] || { echo "no guest image at $ELF - build it first" >&2; exit 2; }
command -v qemu-system-m68k >/dev/null || { echo "qemu-system-m68k not on PATH" >&2; exit 2; }

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

#!/bin/sh
#
# Build the self-contained Windows package.
#
# The version comes from the VERSION file at the repo root and from
# nowhere else. It ends up in four places that must agree -- the output
# filename, the installer's name, Add/Remove Programs, and the title of
# the running QEMU window -- and a build that had to be told the version
# twice would eventually be told two different things.
#
#   tools/make-windows-package.sh [output-dir]
#
# Output: <output-dir>/shinogi-<version>-win64-setup.exe and a .sha256
# beside it. Default output-dir is the LAN share.
#
# The bundle carries two programs of our own: shinogi.exe, the launcher,
# and shinogi-hostfsd.exe, the helper that serves the host folder the
# guest sees as drive C. Both are built here from the same sources the
# Linux and macOS builds use.
#
# Prerequisites, none of which this script installs:
#   - x86_64-w64-mingw32-gcc          (cross compiler)
#   - makensis                        (installer builder)
#   - an extracted QEMU-for-Windows tree, see QEMU_WIN below
#   - $HOME/mingw-sdl2                (SDL2 for the diagnostic probe)
#
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(cat "$ROOT/VERSION")
OUTDIR="${1:-$HOME/git/Aranym/lan-share}"

ELF="${SHINOGI_ELF:-$HOME/git/emutos/emutos-virt.elf}"
QEMU_WIN="${QEMU_WIN:-$HOME/shinogi-build/qemu-w64}"
SDL2="${SDL2_MINGW:-$HOME/mingw-sdl2/x86_64-w64-mingw32}"
BUNDLE="${TMPDIR:-/tmp}/shinogi-win-$VERSION"

[ -n "$VERSION" ] || { echo "VERSION is empty" >&2; exit 2; }
[ -f "$ELF" ]     || { echo "no guest image at $ELF - build it first" >&2; exit 2; }
[ -d "$QEMU_WIN" ] || {
    echo "no QEMU-for-Windows tree at $QEMU_WIN" >&2
    echo "extract the official installer with 7z and set QEMU_WIN=<dir>" >&2
    echo "  https://qemu.weilnetz.de/w64/" >&2
    exit 2
}
command -v x86_64-w64-mingw32-gcc >/dev/null || { echo "mingw cross compiler missing" >&2; exit 2; }
command -v makensis >/dev/null || { echo "makensis missing" >&2; exit 2; }

echo "shinogi $VERSION -> $OUTDIR"

# Assemble the bundle. Only the m68k target is kept: the upstream tree is
# 1.2GB, most of it other architectures and firmware this machine never
# loads. All 114 DLLs are kept deliberately rather than computing the
# import closure -- a missing DLL fails on the user's desktop, where it
# cannot be diagnosed, and the compressed cost of the extras is small.
mkdir -p "$BUNDLE/qemu/share" "$BUNDLE/qemu/lib"
cp "$QEMU_WIN/qemu-system-m68k.exe" "$QEMU_WIN/qemu-system-m68kw.exe" "$BUNDLE/qemu/"
cp "$QEMU_WIN"/*.dll "$BUNDLE/qemu/"
cp -r "$QEMU_WIN/share/keymaps" "$QEMU_WIN/share/icons" "$QEMU_WIN/share/locale" \
      "$BUNDLE/qemu/share/"
cp -r "$QEMU_WIN/lib/." "$BUNDLE/qemu/lib/"
cp "$QEMU_WIN/COPYING" "$QEMU_WIN/COPYING.LIB" "$QEMU_WIN/VERSION" "$BUNDLE/qemu/"

cp "$ELF" "$BUNDLE/"
cp "$SDL2/bin/SDL2.dll" "$BUNDLE/"

x86_64-w64-mingw32-gcc "$ROOT/tools/win/shinogi-launcher.c" \
    -DSHINOGI_VERSION="\"$VERSION\"" \
    -o "$BUNDLE/shinogi.exe" -mwindows -O2 -Wall -Wextra

# The host end of drive C. Console subsystem, but the launcher starts it
# with CREATE_NO_WINDOW so nothing flashes up; -lws2_32 is for the
# AF_UNIX socket it listens on, which Windows serves through Winsock.
x86_64-w64-mingw32-gcc "$ROOT/tools/hostfsd/shinogi-hostfsd.c" \
    -o "$BUNDLE/shinogi-hostfsd.exe" -mconsole -O2 -Wall -Wextra -lws2_32

x86_64-w64-mingw32-gcc "$ROOT/tools/sdl-grab-probe.c" \
    -I"$SDL2/include" -I"$SDL2/include/SDL2" -L"$SDL2/lib" \
    -o "$BUNDLE/sdl-grab-probe.exe" \
    -lmingw32 -lSDL2main -lSDL2 -mconsole -O2 -Wall

OUT="$OUTDIR/shinogi-$VERSION-win64-setup.exe"
mkdir -p "$OUTDIR"
makensis -DBUNDLE="$BUNDLE" -DOUTFILE="$OUT" -DVERSION="$VERSION" \
         "$ROOT/tools/win/shinogi.nsi" | tail -1

( cd "$OUTDIR" && sha256sum "$(basename "$OUT")" > "$(basename "$OUT").sha256" )

echo
echo "built $OUT"
cat "$OUT.sha256"

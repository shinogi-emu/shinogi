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
QEMU_WIN="${QEMU_WIN:-$HOME/shinogi-build/qemu-w64-patched}"
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

# Assemble the bundle from OUR QEMU, cross-built with the patches in
# patches/ -- notably the control-register fix, without which any guest
# that probes the 68060 PCR kills the emulator outright. The stock
# download from qemu.weilnetz.de carries neither patch.
#
# That build is configured for this one job (m68k, SDL, no gtk/vnc/tools),
# so its DLL set is the 13-entry import closure rather than the stock
# tree's 114, and share/ carries only what the guest can reach.
mkdir -p "$BUNDLE/qemu/share" "$BUNDLE/qemu/lib"
cp "$QEMU_WIN/qemu-system-m68k.exe" "$QEMU_WIN/qemu-system-m68kw.exe" "$BUNDLE/qemu/"
cp "$QEMU_WIN"/*.dll "$BUNDLE/qemu/"
# Only what the guest can actually reach. locale/ exists in the stock
# download but not in our own build, which is configured without the
# pieces that would use it, so its absence is not an error.
for d in keymaps icons locale; do
    [ -d "$QEMU_WIN/share/$d" ] && cp -r "$QEMU_WIN/share/$d" "$BUNDLE/qemu/share/"
done
cp -r "$QEMU_WIN/lib/." "$BUNDLE/qemu/lib/"
cp "$QEMU_WIN/COPYING" "$QEMU_WIN/COPYING.LIB" "$QEMU_WIN/VERSION" "$BUNDLE/qemu/"

cp "$ELF" "$BUNDLE/"
cp "$SDL2/bin/SDL2.dll" "$BUNDLE/"

# The icon and manifest. windres is run from tools/win so the .rc can name
# shinogi.ico beside it; the icon is committed rather than generated here,
# because this script also runs on hosts with no rasterizer.
[ -f "$ROOT/tools/win/shinogi.ico" ] || {
    echo "missing tools/win/shinogi.ico - run tools/make-icons.py" >&2
    exit 2
}
( cd "$ROOT/tools/win" \
  && x86_64-w64-mingw32-windres shinogi.rc -O coff -o "$BUNDLE/shinogi-res.o" )

x86_64-w64-mingw32-gcc "$ROOT/tools/win/shinogi-launcher.c" \
    "$BUNDLE/shinogi-res.o" \
    -DSHINOGI_VERSION="\"$VERSION\"" \
    -o "$BUNDLE/shinogi.exe" -mwindows -O2 -Wall -Wextra
python3 -c "import os,sys; os.remove(sys.argv[1])" "$BUNDLE/shinogi-res.o"

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

# Refuse to overwrite a released installer.
#
# The output name comes from VERSION, so building twice without bumping it
# silently replaces the earlier binary -- and a released installer is the
# one thing here that cannot be rebuilt byte-for-byte later.  That has
# already destroyed one: the Aug-6 beta1 was overwritten by a rebuild that
# still said "beta1", and the tested pairing of that exe with its tree is
# no longer reproducible.  Bump VERSION, or pass FORCE=1 if you really do
# mean to replace it.
if [ -e "$OUT" ] && [ -z "${FORCE:-}" ]; then
    echo "refusing to overwrite $OUT" >&2
    echo "bump VERSION (currently $VERSION), or set FORCE=1" >&2
    exit 2
fi
mkdir -p "$OUTDIR"
makensis -DBUNDLE="$BUNDLE" -DOUTFILE="$OUT" -DVERSION="$VERSION" \
         -DICON="$ROOT/tools/win/shinogi.ico" \
         "$ROOT/tools/win/shinogi.nsi" | tail -1

( cd "$OUTDIR" && sha256sum "$(basename "$OUT")" > "$(basename "$OUT").sha256" )

echo
echo "built $OUT"
cat "$OUT.sha256"

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
#   SHINOGI_CPU=m68060 SHINOGI_ELF=<68060-emutos.elf> \
#     SHINOGI_060SP=<060sp.prg> tools/make-windows-package.sh [output-dir]
#
# Output: <output-dir>/Shinogi-040-<version>-win64-setup.exe or
# Shinogi-060-<version>-win64-setup.exe and a
# .sha256 beside it. Default output-dir is the LAN share.
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
CPU="${SHINOGI_CPU:-m68040}"

case "$CPU" in
    m68040|m68060) ;;
    *)
        echo "unsupported SHINOGI_CPU: $CPU" >&2
        echo "choose m68040 or m68060" >&2
        exit 2
        ;;
esac

case "$CPU" in
    m68040) EDITION=Shinogi-040 ;;
    m68060) EDITION=Shinogi-060 ;;
esac

ELF="${SHINOGI_ELF:-$HOME/git/emutos/emutos-virt.elf}"
QEMU_WIN="${QEMU_WIN:-$HOME/shinogi-build/qemu-w64-patched}"
SDL2="${SDL2_MINGW:-$HOME/mingw-sdl2/x86_64-w64-mingw32}"
# The system tree the guest boots into. Shipped as a published artifact
# rather than rebuilt here: tools/make-mint-install.sh reproduces it, but
# several of its inputs are third-party binaries a build machine cannot
# fetch.
DRIVE_C_ZIP="${SHINOGI_DRIVE_C_ZIP:-$HOME/git/Aranym/lan-share/freemint-install.zip}"
NET_DRIVER="${SHINOGI_NET_DRIVER:-$HOME/git/freemint/sys/sockets/xif/virtio_net/.compile_02060/virtio_net.xif}"
SP060="${SHINOGI_060SP:-$HOME/git/freemint/sys/arch/060sp/060sp.prg}"
DEFAULT_KERNEL="$HOME/git/freemint/sys/.compile_hat02060/mint0206h.prg"
KERNEL="${SHINOGI_MINT_KERNEL:-$DEFAULT_KERNEL}"
BUNDLE="${TMPDIR:-/tmp}/shinogi-win-$EDITION-$VERSION"

[ -n "$VERSION" ] || { echo "VERSION is empty" >&2; exit 2; }
if [ "$CPU" = m68060 ] && [ -z "${SHINOGI_ELF:-}" ]; then
    echo "SHINOGI_ELF must name an EmuTOS image compiled for m68060" >&2
    exit 2
fi
[ -f "$ELF" ]     || { echo "no guest image at $ELF - build it first" >&2; exit 2; }
[ -d "$QEMU_WIN" ] || {
    echo "no QEMU-for-Windows tree at $QEMU_WIN" >&2
    echo "extract the official installer with 7z and set QEMU_WIN=<dir>" >&2
    echo "  https://qemu.weilnetz.de/w64/" >&2
    exit 2
}
command -v x86_64-w64-mingw32-gcc >/dev/null || { echo "mingw cross compiler missing" >&2; exit 2; }
command -v makensis >/dev/null || { echo "makensis missing" >&2; exit 2; }
[ -f "$DRIVE_C_ZIP" ] || {
    echo "no drive C system tree at $DRIVE_C_ZIP" >&2
    echo "set SHINOGI_DRIVE_C_ZIP=<freemint-install.zip>" >&2
    exit 2
}

[ -f "$NET_DRIVER" ] || {
    echo "no guest network driver at $NET_DRIVER" >&2
    echo "build FreeMiNT sys/sockets/xif/virtio_net, or set SHINOGI_NET_DRIVER=<file>" >&2
    exit 2
}
[ -f "$KERNEL" ] || {
    echo "no shared 020-060 FreeMiNT kernel at $KERNEL" >&2
    echo "build Shinogi's hat02060 kernel, or set SHINOGI_MINT_KERNEL=<file>" >&2
    exit 2
}
if [ "$CPU" = m68060 ] && [ ! -f "$SP060" ]; then
    echo "no 68060 software package at $SP060" >&2
    echo "build FreeMiNT sys/arch/060sp, or set SHINOGI_060SP=<file>" >&2
    exit 2
fi

echo "$EDITION $VERSION ($CPU) -> $OUTDIR"

# Assemble the bundle from OUR QEMU, cross-built with the patches in
# patches/ -- notably the control-register fix, without which any guest
# that probes the 68060 PCR kills the emulator outright. The stock
# download from qemu.weilnetz.de carries neither patch.
#
# That build is configured for this one job (m68k, SDL, no gtk/vnc/tools),
# so its DLL set is the 13-entry import closure rather than the stock
# tree's 114, and share/ carries only what the guest can reach.
mkdir -p "$BUNDLE/qemu/share" "$BUNDLE/qemu/lib"
cp -f "$QEMU_WIN/qemu-system-m68k.exe" "$QEMU_WIN/qemu-system-m68kw.exe" "$BUNDLE/qemu/"
cp -f "$QEMU_WIN"/*.dll "$BUNDLE/qemu/"
# Only what the guest can actually reach. locale/ exists in the stock
# download but not in our own build, which is configured without the
# pieces that would use it, so its absence is not an error.
for d in keymaps icons locale; do
    [ -d "$QEMU_WIN/share/$d" ] && cp -rf "$QEMU_WIN/share/$d" "$BUNDLE/qemu/share/"
done

# The emulator window and its taskbar button belong to qemu-system-m68k,
# not to our launcher, so giving the launcher an icon never touched them -
# what the user sees is QEMU's own logo.  ui/sdl2.c does not embed that
# icon, it LOADS it from share/icons at startup, so dropping ours over it
# is enough.  Both names are replaced because which one is read depends on
# whether that QEMU was built with SDL_image.
ICONS="$BUNDLE/qemu/share/icons/hicolor"
[ -f "$ICONS/128x128/apps/qemu.png" ] &&
    cp -f "$ROOT/tools/win/qemu-icon.png" "$ICONS/128x128/apps/qemu.png"
[ -f "$ICONS/32x32/apps/qemu.bmp" ] &&
    cp -f "$ROOT/tools/win/qemu-icon.bmp" "$ICONS/32x32/apps/qemu.bmp"
cp -rf "$QEMU_WIN/lib/." "$BUNDLE/qemu/lib/"
cp -f "$QEMU_WIN/COPYING" "$QEMU_WIN/COPYING.LIB" "$QEMU_WIN/VERSION" "$BUNDLE/qemu/"

cp -f "$ELF" "$BUNDLE/emutos-virt.elf"

# Drive C, pristine. The installer lays this down only when the user has
# no drive C yet; an existing one gets the three overlays alone, because
# it holds their files and their edited configs (shin-apn.19).
unzip -q "$DRIVE_C_ZIP" -d "$BUNDLE"
[ -d "$BUNDLE/freemint-install" ] || {
    echo "$DRIVE_C_ZIP did not contain freemint-install/" >&2; exit 2; }
mv "$BUNDLE/freemint-install" "$BUNDLE/drive-c"
if [ "$CPU" = m68060 ]; then
    cp -f "$SP060" "$BUNDLE/drive-c/AUTO/060SP.PRG"
else
    rm -f "$BUNDLE/drive-c/AUTO/060SP.PRG"
fi
cp -f "$KERNEL" "$BUNDLE/drive-c/AUTO/MINT.PRG"
cp -f "$NET_DRIVER" "$BUNDLE/drive-c/MINT/1-19-CUR/VIRTIONE.XIF"
cp -f "$SDL2/bin/SDL2.dll" "$BUNDLE/"
cp -f "$ROOT/tools/win/README.txt" "$BUNDLE/README.txt"

cp -f "$NET_DRIVER" "$BUNDLE/VIRTIONE.XIF"
cp -f "$KERNEL" "$BUNDLE/MINT.PRG"

SP060_DEFINE=
if [ "$CPU" = m68060 ]; then
    cp -f "$SP060" "$BUNDLE/060SP.PRG"
    SP060_DEFINE=-DINCLUDE_060SP
fi

{
    printf '%s %s\r\n' "$EDITION" "$VERSION"
    printf 'Edition: %s\r\n' "$EDITION"
    printf 'CPU: %s\r\n' "$CPU"
    printf 'QEMU: %s\r\n' "$(cat "$QEMU_WIN/VERSION")"
    if [ "$CPU" != m68040 ]; then
        printf 'Distribution: private development build; do not redistribute\r\n'
    fi
    if [ "$CPU" = m68060 ]; then
        printf 'Guest image: 020-060 EmuTOS on m68060\r\n'
        printf 'Compatibility: FreeMiNT/Motorola 68060 software package\r\n'
    fi
    printf 'FreeMiNT kernel: shared 020-060 OLDTOSFS build\r\n'
    printf 'Network driver: bundled 20 ms receive-poll build\r\n'
    # Checksums of the two pieces that decide how the machine behaves.
    # Without them, "which kernel is in this build?" can only be answered
    # by rebuilding and comparing, and a stale one shipped once already.
    printf 'Guest image sha256: %s\r\n' "$(sha256sum "$ELF" | cut -d' ' -f1)"
    printf 'FreeMiNT kernel sha256: %s\r\n' "$(sha256sum "$KERNEL" | cut -d' ' -f1)"
} > "$BUNDLE/BUILD.txt"

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
    -DSHINOGI_EDITION="\"$EDITION\"" \
    -DSHINOGI_CPU="\"$CPU\"" \
    -o "$BUNDLE/shinogi.exe" -mwindows -O2 -Wall -Wextra
python3 -c "import os,sys; os.remove(sys.argv[1])" "$BUNDLE/shinogi-res.o"

# The host end of drive C. Console subsystem, but the launcher starts it
# with CREATE_NO_WINDOW so nothing flashes up; -lws2_32 is for the
# loopback TCP listener it uses on Windows.
x86_64-w64-mingw32-gcc "$ROOT/tools/hostfsd/shinogi-hostfsd.c" \
    -o "$BUNDLE/shinogi-hostfsd.exe" -mconsole -O2 -Wall -Wextra -lws2_32

x86_64-w64-mingw32-gcc "$ROOT/tools/sdl-grab-probe.c" \
    -I"$SDL2/include" -I"$SDL2/include/SDL2" -L"$SDL2/lib" \
    -o "$BUNDLE/sdl-grab-probe.exe" \
    -lmingw32 -lSDL2main -lSDL2 -mconsole -O2 -Wall

OUT="$OUTDIR/$EDITION-$VERSION-win64-setup.exe"

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
# SP060_DEFINE is a flag with no path in it, so intentional word splitting
# here adds either one argument or none.
# shellcheck disable=SC2086
makensis -DBUNDLE="$BUNDLE" -DOUTFILE="$OUT" -DVERSION="$VERSION" \
         -DEDITION="$EDITION" \
         -DCPU="$CPU" \
         -DINCLUDE_NET_DRIVER \
         $SP060_DEFINE \
         -DICON="$ROOT/tools/win/shinogi.ico" \
         "$ROOT/tools/win/shinogi.nsi" | tail -1

( cd "$OUTDIR" && sha256sum "$(basename "$OUT")" > "$(basename "$OUT").sha256" )

echo
echo "built $OUT"
cat "$OUT.sha256"

#!/bin/sh
#
# Build the self-contained Linux package.
#
#   SHINOGI_CPU=m68060 SHINOGI_ELF=<68060-emutos.elf> \
#     SHINOGI_060SP=<060sp.prg> tools/make-linux-package.sh [output-dir]
#
# Output: <output-dir>/Shinogi-060-<version>-linux-x86_64.tar.gz and a
# .sha256 beside it. Default output-dir is the LAN share.
#
# The version comes from the VERSION file and from nowhere else, for the
# same reason the Windows build does it that way.
#
# WHAT MAKES THIS SELF-CONTAINED. QEMU's whole library closure is copied
# in, INCLUDING libc and the dynamic loader. That is unusual and it is
# deliberate: our QEMU binds cfsetispeed/cfsetospeed at GLIBC_2.42 and
# sqrtf at GLIBC_2.43, versions that exist on the machine that builds it
# and on no shipping WSL image, so a bundle carrying only the "interesting"
# libraries would refuse to start on exactly the host it was made for.
# The launcher tries the system loader first and only falls back.
#
# Prerequisites:
#   - a patched Linux qemu-system-m68k build   (QEMU_LINUX)
#   - cc with static libc                      (for the two helpers)
#   - the FreeMiNT drive C zip                 (SHINOGI_DRIVE_C_ZIP)
#
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(cat "$ROOT/VERSION")
OUTDIR="${1:-$HOME/git/Aranym/lan-share}"
CPU="${SHINOGI_CPU:-m68040}"

case "$CPU" in
    m68040) EDITION=Shinogi-040 ;;
    m68060) EDITION=Shinogi-060 ;;
    *) echo "unsupported SHINOGI_CPU: $CPU (choose m68040 or m68060)" >&2; exit 2 ;;
esac

ELF="${SHINOGI_ELF:-$HOME/git/emutos/emutos-virt.elf}"
QEMU_LINUX="${QEMU_LINUX:-$HOME/git/atari-docs/qemu-m68k/build-gui/qemu-system-m68k}"
KEYMAPS="${SHINOGI_KEYMAPS:-$(dirname "$QEMU_LINUX")/pc-bios/keymaps}"
DRIVE_C_ZIP="${SHINOGI_DRIVE_C_ZIP:-$HOME/git/Aranym/lan-share/freemint-install.zip}"
NET_DRIVER="${SHINOGI_NET_DRIVER:-$HOME/git/freemint/sys/sockets/xif/virtio_net/.compile_02060/virtio_net.xif}"
SP060="${SHINOGI_060SP:-$HOME/git/freemint/sys/arch/060sp/060sp.prg}"
KERNEL="${SHINOGI_MINT_KERNEL:-$HOME/git/freemint/sys/.compile_hat02060/mint0206h.prg}"

NAME="$EDITION-$VERSION-linux-x86_64"
BUNDLE="${TMPDIR:-/tmp}/shinogi-linux-$NAME"

[ -n "$VERSION" ] || { echo "VERSION is empty" >&2; exit 2; }
if [ "$CPU" = m68060 ] && [ -z "${SHINOGI_ELF:-}" ]; then
    echo "SHINOGI_ELF must name an EmuTOS image compiled for m68060" >&2
    exit 2
fi
[ -f "$ELF" ]        || { echo "no guest image at $ELF" >&2; exit 2; }
[ -x "$QEMU_LINUX" ] || { echo "no patched Linux QEMU at $QEMU_LINUX" >&2; exit 2; }
[ -d "$KEYMAPS" ]    || { echo "no keymaps at $KEYMAPS" >&2; exit 2; }
[ -f "$DRIVE_C_ZIP" ] || { echo "no drive C tree at $DRIVE_C_ZIP" >&2; exit 2; }
[ -f "$NET_DRIVER" ] || { echo "no guest network driver at $NET_DRIVER" >&2; exit 2; }
[ -f "$KERNEL" ]     || { echo "no 020-060 FreeMiNT kernel at $KERNEL" >&2; exit 2; }
if [ "$CPU" = m68060 ] && [ ! -f "$SP060" ]; then
    echo "no 68060 software package at $SP060" >&2; exit 2
fi
command -v unzip >/dev/null || { echo "unzip missing" >&2; exit 2; }

OUT="$OUTDIR/$NAME.tar.gz"
# Refuse to overwrite a released archive, for the reason recorded in
# make-windows-package.sh: a shipped build cannot be reproduced later.
if [ -e "$OUT" ] && [ -z "${FORCE:-}" ]; then
    echo "refusing to overwrite $OUT" >&2
    echo "bump VERSION (currently $VERSION), or set FORCE=1" >&2
    exit 2
fi

echo "$EDITION $VERSION ($CPU) -> $OUT"

rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/bin" "$BUNDLE/lib" "$BUNDLE/guest" "$BUNDLE/share/qemu"

# --- QEMU and its entire closure ---------------------------------------
# The C library and the loader go in a directory of their OWN, apart from
# everything else, and that separation is the whole point rather than
# tidiness.
#
# The launcher tries the host's loader first. If the bundle's glibc were
# on the search path for that attempt, the host's ld.so would load our
# newer libc.so.6 -- a mismatched loader and C library, which does not
# fail politely with a version message, it SEGFAULTS. That was reported
# from a machine with an older glibc, where the probe crashed and only
# the fallback saved the run. Kept apart, the native attempt sees system
# glibc plus our other libraries and fails cleanly when it cannot work,
# and the bundled attempt names lib/glibc explicitly.
mkdir -p "$BUNDLE/lib/glibc"
cp -f "$QEMU_LINUX" "$BUNDLE/bin/qemu-system-m68k"
ldd "$QEMU_LINUX" | awk '/=> \//{print $3}' | sort -u | while read -r so; do
    case "${so##*/}" in
        libc.so.*|libm.so.*|libmvec.so.*|libpthread.so.*|libdl.so.*|\
        librt.so.*|libresolv.so.*|libnsl.so.*|libutil.so.*|libanl.so.*)
            cp -Lf "$so" "$BUNDLE/lib/glibc/" ;;
        *)
            cp -Lf "$so" "$BUNDLE/lib/" ;;
    esac
done
# The loader is not in ldd's "=>" list; it is the last line, in parentheses.
LOADER=$(ldd "$QEMU_LINUX" | awk '/ld-linux/{gsub(/[()]/,"",$1); print $1; exit}')
[ -n "$LOADER" ] && [ -e "$LOADER" ] || { echo "cannot find the dynamic loader" >&2; exit 2; }
cp -Lf "$LOADER" "$BUNDLE/lib/glibc/ld-linux-x86-64.so.2"
chmod +x "$BUNDLE/lib/glibc/ld-linux-x86-64.so.2"
cp -rf "$KEYMAPS" "$BUNDLE/share/qemu/"

# The monitor helper is static so the launcher can talk to a running
# guest without socat, netcat or python being installed.
cc -static -O2 -Wall -Wextra \
   -o "$BUNDLE/bin/shinogi-monitor" "$ROOT/tools/linux/shinogi-monitor.c"
if file "$BUNDLE/bin/shinogi-monitor" | grep -q 'dynamically linked'; then
    echo "shinogi-monitor did not link statically - install libc6-dev" >&2
    exit 2
fi

# Crop and magnify, so a caller can read a dialog without downscaling
# the screen it sits in. Static, and it carries its own PNG writer rather
# than linking zlib, for the same reason.
cc -static -O2 -Wall -Wextra \
   -o "$BUNDLE/bin/shinogi-shot" "$ROOT/tools/linux/shinogi-shot.c"

# The host end of drive C, the same program the Windows bundle carries.
# Static for the same reason as the helper above: it must not care what
# C library the machine it lands on has.
cc -static -O2 -Wall -Wextra \
   -o "$BUNDLE/bin/shinogi-hostfsd" "$ROOT/tools/hostfsd/shinogi-hostfsd.c"
if file "$BUNDLE/bin/shinogi-hostfsd" | grep -q 'dynamically linked'; then
    echo "shinogi-hostfsd did not link statically - install libc6-dev" >&2
    exit 2
fi

# --- the guest ---------------------------------------------------------
cp -f "$ELF" "$BUNDLE/guest/emutos-virt.elf"

unzip -q "$DRIVE_C_ZIP" -d "$BUNDLE/guest"
[ -d "$BUNDLE/guest/freemint-install" ] || {
    echo "$DRIVE_C_ZIP did not contain freemint-install/" >&2; exit 2; }
mv "$BUNDLE/guest/freemint-install" "$BUNDLE/guest/drive-c"

# The same three overlays the Windows installer applies: the shipped tree
# carries an older kernel and driver, and no 060 software package at all.
cp -f "$KERNEL" "$BUNDLE/guest/drive-c/AUTO/MINT.PRG"
cp -f "$NET_DRIVER" "$BUNDLE/guest/drive-c/MINT/1-19-CUR/VIRTIONE.XIF"
if [ "$CPU" = m68060 ]; then
    cp -f "$SP060" "$BUNDLE/guest/drive-c/AUTO/060SP.PRG"
else
    rm -f "$BUNDLE/guest/drive-c/AUTO/060SP.PRG"
fi

# --- launcher and documentation ----------------------------------------
sed -e "s|@CPU@|$CPU|g" -e "s|@EDITION@|$EDITION|g" -e "s|@VERSION@|$VERSION|g" \
    "$ROOT/tools/linux/shinogi.sh" > "$BUNDLE/shinogi"
chmod +x "$BUNDLE/shinogi"
grep -q '@CPU@\|@EDITION@\|@VERSION@' "$BUNDLE/shinogi" &&
    { echo "launcher still has unsubstituted placeholders" >&2; exit 2; }

cp -f "$ROOT/tools/linux/README.txt" "$BUNDLE/README.txt"

{
    echo "$EDITION $VERSION"
    echo "Edition: $EDITION"
    echo "CPU: $CPU"
    echo "Host: linux-x86_64"
    echo "QEMU: $("$QEMU_LINUX" --version | head -1)"
    echo "Built against glibc: $(ldd --version | head -1 | sed 's/^ldd //')"
    if [ "$CPU" != m68040 ]; then
        echo "Distribution: private development build; do not redistribute"
    fi
    if [ "$CPU" = m68060 ]; then
        echo "Guest image: 020-060 EmuTOS on m68060"
        echo "Compatibility: FreeMiNT/Motorola 68060 software package"
    fi
    echo "FreeMiNT kernel: shared 020-060 OLDTOSFS build"
    echo "Network driver: bundled 20 ms receive-poll build"
    echo "EmuTOS image sha256: $(sha256sum "$ELF" | cut -d' ' -f1)"
    echo "FreeMiNT kernel sha256: $(sha256sum "$KERNEL" | cut -d' ' -f1)"
} > "$BUNDLE/BUILD.txt"

# --- archive -----------------------------------------------------------
mkdir -p "$OUTDIR"
STAGE="${TMPDIR:-/tmp}/shinogi-linux-stage-$$"
rm -rf "$STAGE"
mkdir -p "$STAGE"
mv "$BUNDLE" "$STAGE/$NAME"
tar -C "$STAGE" -czf "$OUT" "$NAME"
rm -rf "$STAGE"

( cd "$OUTDIR" && sha256sum "$(basename "$OUT")" > "$(basename "$OUT").sha256" )

echo
echo "built $OUT"
ls -l "$OUT"
cat "$OUT.sha256"

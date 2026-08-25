#!/bin/sh
#
# Build shinogi.app for macOS.
#
#   SHINOGI_CPU=m68060 SHINOGI_ELF=<68020-60-emutos.elf> \
#     SHINOGI_060SP=<060sp.prg> tools/make-macos-package.sh [output-dir]
#
# Output: <output-dir>/Shinogi-040-<version>-macos-arm64.zip or
# Shinogi-060-<version>-macos-arm64.zip
#
# Produces <output-dir>/shinogi.app containing the guest, the launcher,
# and a relocated copy of QEMU with its dylibs, so the app runs on a
# machine with no Homebrew.
#
# UNVERIFIED: written without access to a Mac. The structure and the
# dylib relocation follow standard practice, but the first real run is
# the test. Expect the failures to be a missing dylib or a signature
# that does not cover something -- both show up at launch, not at build.
#
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(cat "$ROOT/VERSION")
OUTDIR="${1:-$ROOT/dist}"
ELF="${SHINOGI_ELF:-$HOME/git/emutos/emutos-virt.elf}"

CPU="${SHINOGI_CPU:-m68040}"
case "$CPU" in
    m68040) EDITION=Shinogi-040 ;;
    m68060) EDITION=Shinogi-060 ;;
    *) echo "SHINOGI_CPU must be m68040 or m68060, not $CPU" >&2; exit 2 ;;
esac

# The system tree the guest boots into: EmuTOS, fVDI, FreeMiNT, XaAES and
# the bundled GEM applications. Shipped as a published artifact rather
# than rebuilt here, because several of its inputs are third-party
# binaries a build machine cannot fetch. tools/make-mint-install.sh
# reproduces it.
DRIVE_C_ZIP="${SHINOGI_DRIVE_C_ZIP:-$HOME/git/Aranym/lan-share/freemint-install.zip}"
NET_DRIVER="${SHINOGI_NET_DRIVER:-$HOME/git/freemint/sys/sockets/xif/virtio_net/.compile_02060/virtio_net.xif}"
KERNEL="${SHINOGI_MINT_KERNEL:-$HOME/git/freemint/sys/.compile_hat02060/mint0206h.prg}"
SP060="${SHINOGI_060SP:-$HOME/git/freemint/sys/arch/060sp/060sp.prg}"

for f in "$DRIVE_C_ZIP" "$NET_DRIVER" "$KERNEL"; do
    [ -f "$f" ] || { echo "missing input: $f" >&2; exit 2; }
done
if [ "$CPU" = m68060 ] && [ ! -f "$SP060" ]; then
    echo "no 68060 software package at $SP060" >&2; exit 2
fi

APP="$OUTDIR/shinogi.app"
MACOS="$APP/Contents/MacOS"
RES="$APP/Contents/Resources"

[ -f "$ELF" ] || { echo "no guest image at $ELF" >&2; exit 2; }
command -v qemu-system-m68k >/dev/null || {
    echo "qemu-system-m68k not found" >&2
    echo "build it from shinogi-emu/qemu-m68k and put its bin on PATH" >&2
    exit 2
}

# Refuse a Homebrew emulator. Its qemu is stock upstream, and the 060
# edition panics on it the moment the guest reaches 060SP.PRG - "Line F
# Emulator", the 68060 unimplemented-FPU trap - because it does not carry
# the project's 68060 work. That shipped once, signed and notarised and
# unable to boot, so it is caught here rather than by a tester.
#
# The test is WHERE the binary came from, not what it reports. A build
# from a shallow clone has no git describe output, so the version string
# alone cannot tell the two apart - which is how this check failed the
# first time it ran.
case "$(command -v qemu-system-m68k)" in
    /opt/homebrew/*|/usr/local/Cellar/*|/usr/local/bin/*)
        echo "refusing Homebrew's qemu: $(command -v qemu-system-m68k)" >&2
        echo "build shinogi-emu/qemu-m68k and put its bin first on PATH" >&2
        echo "the 060 edition cannot boot on a stock QEMU - 'Line F Emulator'" >&2
        exit 2 ;;
esac

QEMU_BIN=$(command -v qemu-system-m68k)
QEMU_PREFIX=$(cd "$(dirname "$QEMU_BIN")/.." && pwd)

echo "$EDITION $VERSION ($CPU) -> $APP"
echo "  qemu: $QEMU_BIN"

rm -rf "$APP"
mkdir -p "$MACOS" "$RES" "$MACOS/qemu/bin" "$MACOS/qemu/lib" "$RES/qemu/share"

# Resources, not MacOS.  Contents/MacOS is for Mach-O, and codesign
# treats anything nested there as code that must itself carry a
# signature -- which an m68k ELF cannot.  Left in MacOS it signs every
# dylib cleanly and then fails the bundle with "code object is not
# signed at all".
cp "$ELF" "$RES/emutos-virt.elf"

# Drive C ships pristine inside the bundle and the launcher copies it out
# on first run, so the folder the user edits is never inside an .app that
# the next release replaces. Same arrangement as the Linux bundle.
unzip -q "$DRIVE_C_ZIP" -d "$RES"
[ -d "$RES/freemint-install" ] || {
    echo "$DRIVE_C_ZIP did not contain freemint-install/" >&2; exit 2; }
mv "$RES/freemint-install" "$RES/drive-c"

# The tree carries an older kernel and driver and no 060 package, so the
# current ones go over the top - the same three overlays the Windows and
# Linux packagers apply.
# EMUTOS.IMG too. The launcher prefers drive C's copy over the bundled
# guest -- that is the documented way a user swaps firmware -- so leaving
# the tree's own copy in place means the edition's guest image is built,
# shipped and then never booted. It only looked right because the tree
# carried an 020-60 image, which runs on both CPUs.
cp -f "$ELF" "$RES/drive-c/EMUTOS.IMG"
cp -f "$KERNEL" "$RES/drive-c/AUTO/MINT.PRG"
cp -f "$NET_DRIVER" "$RES/drive-c/MINT/1-19-CUR/VIRTIONE.XIF"
if [ "$CPU" = m68060 ]; then
    cp -f "$SP060" "$RES/drive-c/AUTO/060SP.PRG"
else
    rm -f "$RES/drive-c/AUTO/060SP.PRG"
fi

# The drive C helper. Without it the launcher has no drive C at all, the
# AUTO folder never runs, and what comes up is the bare EmuTOS desktop -
# which reads as a broken build rather than a missing helper. It lands in
# Contents/MacOS because it is Mach-O, and sign-macos.sh signs every
# executable there before sealing the bundle.
cc "$ROOT/tools/hostfsd/shinogi-hostfsd.c" \
   -o "$MACOS/shinogi-hostfsd" -O2 -Wall -Wextra
cp "$QEMU_BIN" "$MACOS/qemu/bin/"

# QEMU's data files, in Resources for the same reason as the guest image:
# codesign refuses a bundle with unsigned non-code under Contents/MacOS,
# and it names only the first file it trips over, so these surfaced one
# failure later than the ELF did.  The launcher points QEMU at them with
# -L, since they are no longer where it would look by itself.
for d in keymaps; do
    [ -d "$QEMU_PREFIX/share/qemu/$d" ] && cp -R "$QEMU_PREFIX/share/qemu/$d" "$RES/qemu/share/"
done

# Relocate the dylibs. Homebrew's binary references /opt/homebrew paths,
# which do not exist on a machine without Homebrew, so every dependency
# is copied in and every reference rewritten to @executable_path.
if ! command -v dylibbundler >/dev/null; then
    echo "dylibbundler not found - brew install dylibbundler" >&2
    exit 2
fi
dylibbundler -od -b \
    -x "$MACOS/qemu/bin/qemu-system-m68k" \
    -d "$MACOS/qemu/lib" \
    -p "@executable_path/../lib" \
    -s "$QEMU_PREFIX/lib" > /dev/null

cc "$ROOT/tools/shinogi-launcher.c" \
   -DSHINOGI_VERSION="\"$VERSION\"" \
   -DSHINOGI_CPU="\"$CPU\"" \
   -DSHINOGI_EDITION="\"$EDITION\"" \
   -o "$MACOS/shinogi" -O2 -Wall

# The icon, converted from the PNG set the Linux build already uses.
if command -v iconutil >/dev/null && command -v sips >/dev/null; then
    ICONSET="$OUTDIR/shinogi.iconset"
    rm -rf "$ICONSET"; mkdir -p "$ICONSET"
    sips -z 16 16   "$ROOT/tools/linux/shinogi.png" --out "$ICONSET/icon_16x16.png"      > /dev/null
    sips -z 32 32   "$ROOT/tools/linux/shinogi.png" --out "$ICONSET/icon_16x16@2x.png"   > /dev/null
    sips -z 32 32   "$ROOT/tools/linux/shinogi.png" --out "$ICONSET/icon_32x32.png"      > /dev/null
    sips -z 64 64   "$ROOT/tools/linux/shinogi.png" --out "$ICONSET/icon_32x32@2x.png"   > /dev/null
    sips -z 128 128 "$ROOT/tools/linux/shinogi.png" --out "$ICONSET/icon_128x128.png"    > /dev/null
    sips -z 256 256 "$ROOT/tools/linux/shinogi.png" --out "$ICONSET/icon_128x128@2x.png" > /dev/null
    sips -z 256 256 "$ROOT/tools/linux/shinogi.png" --out "$ICONSET/icon_256x256.png"    > /dev/null
    iconutil -c icns "$ICONSET" -o "$RES/shinogi.icns"
    rm -rf "$ICONSET"
fi

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key>              <string>shinogi</string>
  <key>CFBundleDisplayName</key>       <string>shinogi</string>
  <key>CFBundleIdentifier</key>        <string>no.shinogi.emulator</string>
  <key>CFBundleVersion</key>           <string>$VERSION</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleExecutable</key>        <string>shinogi</string>
  <key>CFBundleIconFile</key>          <string>shinogi</string>
  <key>CFBundlePackageType</key>       <string>APPL</string>
  <key>LSMinimumSystemVersion</key>    <string>11.0</string>
  <key>NSHighResolutionCapable</key>   <true/>
</dict>
</plist>
PLIST

echo "built $APP"

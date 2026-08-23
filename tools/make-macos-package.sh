#!/bin/sh
#
# Build shinogi.app for macOS.
#
#   tools/make-macos-package.sh [output-dir]
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

APP="$OUTDIR/shinogi.app"
MACOS="$APP/Contents/MacOS"
RES="$APP/Contents/Resources"

[ -f "$ELF" ] || { echo "no guest image at $ELF" >&2; exit 2; }
command -v qemu-system-m68k >/dev/null || { echo "qemu-system-m68k not found - brew install qemu" >&2; exit 2; }

QEMU_BIN=$(command -v qemu-system-m68k)
QEMU_PREFIX=$(cd "$(dirname "$QEMU_BIN")/.." && pwd)

echo "shinogi $VERSION -> $APP"
echo "  qemu: $QEMU_BIN"

rm -rf "$APP"
mkdir -p "$MACOS" "$RES" "$MACOS/qemu/bin" "$MACOS/qemu/lib" "$MACOS/qemu/share"

# Resources, not MacOS.  Contents/MacOS is for Mach-O, and codesign
# treats anything nested there as code that must itself carry a
# signature -- which an m68k ELF cannot.  Left in MacOS it signs every
# dylib cleanly and then fails the bundle with "code object is not
# signed at all".
cp "$ELF" "$RES/emutos-virt.elf"
cp "$QEMU_BIN" "$MACOS/qemu/bin/"

# QEMU needs its data files: the m68k target loads keymaps, and the
# cocoa UI wants the share tree present even when it uses little of it.
for d in keymaps; do
    [ -d "$QEMU_PREFIX/share/qemu/$d" ] && cp -R "$QEMU_PREFIX/share/qemu/$d" "$MACOS/qemu/share/"
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

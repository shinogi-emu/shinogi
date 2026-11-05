#!/bin/sh
# Exercise the extracted release, including its bundled loader and SDL.
set -eu
PACKAGE=$(cd "${1:?usage: smoke-linux.sh extracted-bundle}" && pwd)
TEST_HOME=$(mktemp -d)
export SHINOGI_HOME="$TEST_HOME/home with ' apostrophe"
EDITION=$(sed -n 's/^Edition: //p' "$PACKAGE/BUILD.txt")
EXPECTED="$SHINOGI_HOME/drive-c"
LAUNCHER="$PACKAGE/shinogi"
cleanup() {
    "$LAUNCHER" stop --now >/dev/null 2>&1 || true
    # Keep failed smoke logs in the CI output before removing the fixture.
    for name in serial qemu hostfsd; do
        tail -25 "$SHINOGI_HOME/logs/$name.log" 2>/dev/null || true
    done
    rm -rf "$TEST_HOME"
}
trap cleanup EXIT
[ "$(unset SHINOGI_HOME; "$LAUNCHER" drive-c)" = "$HOME/$EDITION/drive-c" ]
[ "$("$LAUNCHER" drive-c)" = "$EXPECTED" ]
for mode in native bundled; do
    export SHINOGI_LOADER="$mode"
    "$LAUNCHER" doctor
    if [ "$mode" = native ]; then display=none; else display=sdl; fi
    "$LAUNCHER" start --display "$display"
    sleep 20
    "$LAUNCHER" status | grep '^running '
    SERIAL="$SHINOGI_HOME/logs/serial.log"
    grep -q '^hostfs:' "$SERIAL"
    if grep -qi 'Panic:' "$SERIAL"; then echo 'guest panicked' >&2; exit 1; fi
    "$LAUNCHER" screenshot "$TEST_HOME/$mode.png"
    python3 - "$TEST_HOME/$mode.png" <<'PY'
from PIL import Image
import sys
image = Image.open(sys.argv[1]).convert('RGB')
assert image.size == (1280, 720), image.size
assert len(image.getcolors(image.width * image.height)) > 8, 'blank framebuffer'
print('PASS rendered guest framebuffer:', image.size)
PY
    "$LAUNCHER" stop --now
    "$LAUNCHER" status | grep '^stopped$'
done
printf 'PASS packaged Linux boot, hostfs, native/bundled loaders and SDL: %s\n' "$EDITION"

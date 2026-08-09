#!/usr/bin/env bash
#
# run-stack.sh - boot the shipping system tree headless and screenshot it.
#
# The tree is assembled from the output of make-mint-install.sh, so what is
# tested is what is shipped.  That is the whole point of this script: the
# previous harness ran against a hand-edited copy under /home/rob/tmp that
# drifted away from the bundle, and the drift was eventually mistaken for a
# QEMU bug (shin-63c) that cost a day.  Never point this at a tree that was
# edited by hand -- rebuild it instead, it takes seconds.
#
#   ./run-stack.sh                  assemble and boot, tag "stack"
#   TAG=fonts BOOT_WAIT=200 ./run-stack.sh
#   KEEP_TREE=1 ./run-stack.sh      reuse the tree from the last run
#
# Output lands in $OUT (default ~/shinogi-build/stackout).  Read the .ppm
# with a CROP, never a resize: small bitmap text aliases into garbage when
# scaled and has twice produced false "rendering is broken" findings.
set -eu

INSTALL="${INSTALL_DIR:-$HOME/shinogi-build/mint-install}"
EXTRA="${EXTRA_APPS:-$HOME/tmp/fvdistack/HIGHWIRE}"
TREE="${STACK_TREE:-$HOME/shinogi-build/stacktree}"
OUT="${STACK_OUT:-$HOME/shinogi-build/stackout}"
TAG="${TAG:-stack}"
BOOT="${BOOT_WAIT:-160}"
QEMU="${SHINOGI_QEMU:-$HOME/git/atari-docs/qemu-m68k/build-vvfat/qemu-system-m68k}"
ELF="${SHINOGI_ELF:-$HOME/git/emutos/emutos-virt.elf}"
HOSTFSD="${SHINOGI_HOSTFSD:-$HOME/git/shinogi/tools/hostfsd/shinogi-hostfsd}"

for f in "$QEMU" "$ELF" "$HOSTFSD"; do
    [ -x "$f" ] || [ -f "$f" ] || { echo "missing: $f" >&2; exit 2; }
done
[ -d "$INSTALL" ] || {
    echo "no install tree at $INSTALL - run tools/make-mint-install.sh first" >&2
    exit 2
}

S="$OUT/$TAG.sock"
L="$OUT/$TAG-serial.log"
Q="$OUT/$TAG.qmp"
P="$OUT/$TAG.ppm"

mkdir -p "$OUT"
python3 - "$S" "$L" "$Q" <<'PYX'
import os, sys
for p in sys.argv[1:]:
    try: os.remove(p)
    except OSError: pass
PYX

# Assemble the tree.  Note the EmuTOS image is passed to -kernel from its
# build location, not from the tree; the copy inside the tree is what the
# Windows launcher would hand to QEMU and is left alone here.
if [ -z "${KEEP_TREE:-}" ]; then
    python3 - "$INSTALL" "$TREE" <<'PYX'
import os, shutil, sys
src, dst = sys.argv[1], sys.argv[2]
if os.path.exists(dst):
    shutil.rmtree(dst)
shutil.copytree(src, dst)
PYX
    if [ -d "$EXTRA" ]; then
        cp -r "$EXTRA" "$TREE"/
        echo "tree: $INSTALL + $(basename "$EXTRA")"
    else
        echo "tree: $INSTALL (no extra apps at $EXTRA)"
    fi

    # Headless means nothing can click, so anything past the desktop has to
    # start itself.  XaAES's "run" does that.  Without this the harness can
    # only prove the desktop comes up, which is not enough to exercise the
    # font path -- fVDI does not touch a face until an application asks for
    # one.  AUTORUN_APP is a guest path, e.g. c:\highwire\highwire.app
    if [ -n "${AUTORUN_APP:-}" ]; then
        CNF="$TREE/MINT/1-19-CUR/XAAES/XAAES.CNF"
        if [ -f "$CNF" ]; then
            printf '\r\nrun %s\r\n' "$AUTORUN_APP" >> "$CNF"
            echo "autorun: $AUTORUN_APP"
        else
            echo "autorun: no XAAES.CNF at $CNF, skipped" >&2
        fi
    fi
else
    echo "tree: reusing $TREE"
fi

"$QEMU" -M virt -m 128 \
    -kernel "$ELF" \
    -device virtio-gpu-device \
    -device virtio-keyboard-device \
    -device virtio-tablet-device \
    -chardev "socket,id=hostfs,path=$S,server=on,wait=off" \
    -device virtio-serial-device \
    -device virtserialport,chardev=hostfs,name=shinogi.hostfs \
    -display none -serial "file:$L" -qmp "unix:$Q,server=on,wait=off" &
QP=$!

w=0
while [ $w -lt 200 ]; do
    [ -S "$S" ] && break
    sleep 0.05
    w=$((w + 1))
done

"$HOSTFSD" --root "$TREE" --connect "$S" > "$OUT/$TAG-hostfsd.log" 2>&1 &
HP=$!

# Poll rather than sleep the full BOOT_WAIT, so a QEMU that dies -- which is
# what an abort looks like from here -- is noticed at once instead of after
# the timeout.
w=0
while [ $w -lt "$BOOT" ]; do
    kill -0 $QP 2>/dev/null || { echo "!!! QEMU EXITED EARLY after ${w}s - abort or panic" >&2; break; }
    sleep 1
    w=$((w + 1))
done

if kill -0 $QP 2>/dev/null; then
    python3 - "$Q" "$P" <<'PYX' || echo "(screendump failed)"
import json, socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
f = s.makefile('rw'); f.readline()
f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush(); f.readline()
f.write(json.dumps({"execute": "screendump", "arguments": {"filename": sys.argv[2]}}) + "\n"); f.flush()
while True:
    l = json.loads(f.readline())
    if "return" in l or "error" in l:
        print(json.dumps(l)); break
PYX
fi

kill $HP $QP 2>/dev/null || true
wait 2>/dev/null || true

echo "===== QEMU survived? ====="
if grep -qiE "insn_opsize|should not be reached|Bail out" "$OUT/$TAG-hostfsd.log" 2>/dev/null; then
    echo "NO - QEMU aborted, see $OUT/$TAG-hostfsd.log"
else
    echo "yes"
fi
echo "===== AUTO scan (fsfirst FVDI.PRG must be followed by fsnext MINT.PRG) ====="
grep -nE "fsfirst FVDI.PRG|fsnext MINT.PRG" "$L" || echo "(AUTO scan did not resume)"
echo "===== scan table pressure (shin-5gy) ====="
grep -c "scan table full" "$L" || true
echo "===== font files re-read (shin-vfz; should be near zero with filecache) ====="
grep -c "fsfirst.*\.TTF" "$L" || true
echo "===== serial: $L ($(wc -l < "$L") lines) ====="
echo "===== screenshot: $P ====="

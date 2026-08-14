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
#   PROFILE=1 ./run-stack.sh        rank executed opcodes and hot TCG blocks
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
CPU="${SHINOGI_CPU:-m68040}"
# Overridable so a harness can point the same tree at another machine
# type.  The shipping default stays virt.
MACHINE="${SHINOGI_MACHINE:-virt}"
ELF="${SHINOGI_ELF:-$HOME/git/emutos/emutos-virt.elf}"
HOSTFSD="${SHINOGI_HOSTFSD:-$HOME/git/shinogi/tools/hostfsd/shinogi-hostfsd}"
SP060="${SHINOGI_060SP:-$HOME/git/freemint/sys/arch/060sp/060sp.prg}"
QEMU_EXTRA="${SHINOGI_QEMU_EXTRA:-}"

if [ -n "${PROFILE:-}" ]; then
    QEMU_BUILD=$(dirname "$QEMU")
    HOWVEC="$QEMU_BUILD/contrib/plugins/libhowvec.so"
    HOTBLOCKS="$QEMU_BUILD/contrib/plugins/libhotblocks.so"
    PROFILE_LOG="$OUT/$TAG-cpu-profile.log"

    for f in "$HOWVEC" "$HOTBLOCKS"; do
        [ -f "$f" ] || {
            echo "missing QEMU profiling plugin: $f" >&2
            exit 2
        }
    done
    QEMU_EXTRA="$QEMU_EXTRA -plugin $HOWVEC,inline=true"
    QEMU_EXTRA="$QEMU_EXTRA -plugin $HOTBLOCKS,inline=true,limit=100"
    QEMU_EXTRA="$QEMU_EXTRA -d plugin -D $PROFILE_LOG"
    echo "cpu profile: opcode histogram + hot blocks -> $PROFILE_LOG"
fi

for f in "$QEMU" "$ELF" "$HOSTFSD"; do
    [ -x "$f" ] || [ -f "$f" ] || { echo "missing: $f" >&2; exit 2; }
done
[ -d "$INSTALL" ] || {
    echo "no install tree at $INSTALL - run tools/make-mint-install.sh first" >&2
    exit 2
}
if [ "$CPU" = m68060 ] && [ ! -f "$SP060" ]; then
    echo "missing 68060 software package: $SP060" >&2
    exit 2
fi

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

    if [ "$CPU" = m68060 ]; then
        cp -f "$SP060" "$TREE/AUTO/060SP.PRG"
        echo "cpu: m68060 with 060SP.PRG compatibility handler"
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

# NET=1 adds the slirp link the shipped launchers give the guest.  Without
# it this harness has no network device AT ALL, and that is not what the
# user runs -- so an application that waits on the network at startup hangs
# here and nowhere else.  That cost a wrong bug report: hw-dom.app was
# filed as "never opens its window" when it opens fine on a machine that
# has a link.  Turn this on before concluding anything about an app that
# talks to the network.
if [ -n "${NET:-}" ]; then
    NETARGS="-netdev user,id=n0,ipv6=off -device virtio-net-device,netdev=n0"
    # Count frames on the HOST, never ask the guest whether it has a
    # network.  An empty capture is 24 bytes, and that is what turns "the
    # app did nothing" into "nothing reached the wire" -- the distinction
    # that made the virtio-net bug diagnosable in the first place.
    NETARGS="$NETARGS -object filter-dump,id=dump0,netdev=n0,file=$OUT/$TAG.pcap"
    echo "net: slirp (10.0.2.15/.2/.3), pcap -> $OUT/$TAG.pcap"
else
    NETARGS=""
fi

# QEMU_EXTRA is a diagnostic-only list of QEMU arguments supplied by the
# caller. Intentional splitting lets it contain more than one option.
# shellcheck disable=SC2086
"$QEMU" -M "$MACHINE" -cpu "$CPU" -m 128 \
    -kernel "$ELF" \
    -device virtio-gpu-device,id=vgpu \
    -device virtio-keyboard-device \
    -device virtio-tablet-device \
    $NETARGS \
    -chardev "socket,id=hostfs,path=$S,server=on,wait=off" \
    -device virtio-serial-device \
    -device virtserialport,chardev=hostfs,name=shinogi.hostfs \
    -display none -serial "file:$L" -qmp "unix:$Q,server=on,wait=off" \
    $QEMU_EXTRA &
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
# Name the console explicitly.  A machine carrying a second display device
# registers it as console 0, and it stays black until a guest driver programs
# it, so a screendump without a device silently captures that instead of the
# desktop.
f.write(json.dumps({"execute": "screendump",
                    "arguments": {"filename": sys.argv[2],
                                  "device": "vgpu"}}) + "\n"); f.flush()
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

#!/bin/sh
#
# Boot the guest and check its serial output against a golden file.
#
# Usage: tools/run-golden.sh <name> <grep-ere> [host-folder]
#
#   <name>      tests/golden/<name>.expected holds the expected lines
#   <grep-ere>  extended regex selecting the lines to compare
#
# Exit codes:
#   0 = the extracted lines match the golden file exactly, in order
#   1 = the guest ran but the extracted lines did not match (diff printed)
#   2 = the test could not be run at all (bad setup, QEMU never produced
#       output, or QEMU exited early) - this is never reported as pass
#       or fail, since a harness failure must never look like a test
#       result
#
set -eu

NAME="${1:?usage: run-golden.sh <name> <grep-ere> [host-folder]}"
PATTERN="${2:?usage: run-golden.sh <name> <grep-ere> [host-folder]}"
FOLDER="${3:-/tmp/shinogi-hostfs}"

ROOT=$(cd "$(dirname "$0")/.." && pwd)
ELF="${SHINOGI_ELF:-$HOME/git/emutos/emutos-virt.elf}"
GOLDEN="$ROOT/tests/golden/$NAME.expected"
WORK="${TMPDIR:-/tmp}/run-golden-$NAME"
BOOT_WAIT="${BOOT_WAIT:-25}"

[ -f "$ELF" ]    || { echo "no guest image at $ELF" >&2; exit 2; }
[ -f "$GOLDEN" ] || { echo "no golden file at $GOLDEN" >&2; exit 2; }
[ -s "$GOLDEN" ] || { echo "golden file $GOLDEN is empty" >&2; exit 2; }

mkdir -p "$WORK" "$FOLDER"
LOG="$WORK/serial.log"
rm -f "$LOG"

qemu-system-m68k \
    -M virt -m 128 \
    -kernel "$ELF" \
    -device virtio-gpu-device \
    -fsdev "local,id=hostfs,path=$FOLDER,security_model=mapped-xattr" \
    -device virtio-9p-device,fsdev=hostfs,mount_tag=shinogi \
    -display none \
    -serial "file:$LOG" \
    -d guest_errors -D "$WORK/guest-errors.log" &
QPID=$!

# The guest never exits on its own, so it has to be stopped from here.
#
# Waiting out the full window every time is what this loop avoids: most
# boots produce their output in a fraction of BOOT_WAIT, and with several
# goldens per verification pass the fixed sleep dominates the run.
#
# Stopping the moment the output matches would be wrong, though: a golden
# can match early and then be spoiled by a later spurious line, which is
# exactly the kind of bug worth catching. So once the extracted output
# matches, wait SETTLE seconds and require it to still match, unchanged,
# before believing it.
#
# QEMU dying on its own (bad device option, invalid kernel, sandbox
# refusal) also breaks the loop early - there is nothing to wait for.
SETTLE="${SETTLE:-3}"
i=0
matched_at=""
while [ "$i" -lt "$BOOT_WAIT" ]; do
    if ! kill -0 "$QPID" 2>/dev/null; then
        break
    fi

    if [ -s "$LOG" ]; then
        grep -aoE "$PATTERN" "$LOG" 2>/dev/null | tr -d '\r' > "$WORK/probe" || true
        if cmp -s "$GOLDEN" "$WORK/probe"; then
            if [ -z "$matched_at" ]; then
                matched_at="$i"
            elif [ "$((i - matched_at))" -ge "$SETTLE" ]; then
                break           # matched and stayed matched
            fi
        else
            matched_at=""       # changed again; keep waiting
        fi
    fi

    sleep 1
    i=$((i + 1))
done
kill "$QPID" 2>/dev/null || true

set +e
wait "$QPID"
QSTATUS=$?
set -e

# A clean SIGTERM shutdown (our own kill above) reports 143; anything
# else non-zero means QEMU exited on its own, almost certainly with an
# error, before we stopped it.
if [ "$QSTATUS" -ne 0 ] && [ "$QSTATUS" -ne 143 ]; then
    echo "qemu-system-m68k exited early with status $QSTATUS - see $WORK/guest-errors.log" >&2
    exit 2
fi

[ -s "$LOG" ] || { echo "no serial output captured in $LOG - guest never ran" >&2; exit 2; }

# The guest emits CRLF; strip the CR so goldens can be plain LF.
grep -aoE "$PATTERN" "$LOG" | tr -d '\r' > "$WORK/actual" || true

if diff -u "$GOLDEN" "$WORK/actual"; then
    echo "PASS $NAME"
    exit 0
fi

echo "FAIL $NAME - see $LOG" >&2
exit 1

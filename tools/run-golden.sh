#!/bin/sh
#
# Boot the guest and check its serial output against a golden file.
#
# Usage: tools/run-golden.sh <name> <grep-ere> [host-folder] [vvfat-folder]
#
#   <name>         tests/golden/<name>.expected holds the expected lines
#   <grep-ere>     extended regex selecting the lines to compare
#   <host-folder>  exported over 9P as drive C
#   <vvfat-folder> exported as a virtio-blk device via QEMU's vvfat
#                  driver, if given; omitted, no block device is
#                  attached at all
#
# The vvfat folder is OPTIONAL and off by default, which is not tidiness:
# attaching the device consumes a virtio-mmio transport slot and shifts
# the slot the 9P device lands in, and tests/golden/phase5-attach pins
# that slot number. Only the goldens that need the block device get it.
#
# GOLDEN_SORTED=1 compares the extracted lines as a SET rather than as a
# sequence: the first <n> matching lines are still the ones taken, but
# both sides are sorted before the diff.
#
# This is for goldens whose lines are in raw host readdir() order, which
# POSIX does not define and which really does differ between hosts -- a
# folder built on tmpfs lists in creation order, the same folder on ext4
# with dir_index lists in hash order. Asserting that order asserts a
# property of the developer's filesystem, and every such golden fails on
# any machine whose /tmp is not the same kind of filesystem.
#
# Sorting drops ONLY the ordering claim. The lines are still compared
# one for one, so a missing entry, an extra entry, a misspelled name or
# a repeated entry all still fail -- a repeat pushes a real line out of
# the first <n> and the sorted sets then differ. Do not reach for it to
# quiet a golden over output the GUEST sorted: that order is the guest's
# own and is the thing under test.
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
VVFAT="${4:-}"

ROOT=$(cd "$(dirname "$0")/.." && pwd)
ELF="${SHINOGI_ELF:-$HOME/git/emutos/emutos-virt.elf}"
GOLDEN="$ROOT/tests/golden/$NAME.expected"
WORK="${TMPDIR:-/tmp}/run-golden-$NAME"
BOOT_WAIT="${BOOT_WAIT:-25}"
GOLDEN_SORTED="${GOLDEN_SORTED:-0}"

[ -f "$ELF" ]    || { echo "no guest image at $ELF" >&2; exit 2; }
[ -f "$GOLDEN" ] || { echo "no golden file at $GOLDEN" >&2; exit 2; }
[ -s "$GOLDEN" ] || { echo "golden file $GOLDEN is empty" >&2; exit 2; }

# How many matching lines the golden expects.
WANT=$(wc -l < "$GOLDEN")

# The side of the comparison the golden file supplies. Sorted in the C
# collation, so the answer does not depend on the runner's locale.
EXPECT="$WORK/expected"
mkdir -p "$WORK"
if [ "$GOLDEN_SORTED" = "1" ]; then
    LC_ALL=C sort "$GOLDEN" > "$EXPECT"
else
    cat "$GOLDEN" > "$EXPECT"
fi

# Pull the lines this golden is about out of the serial log, into $1.
#
# head runs BEFORE any sort, deliberately: "the first $WANT matching
# lines" has to mean the first the guest emitted, not the $WANT that
# happen to sort lowest, or a repeated early line could displace a later
# one without the comparison noticing.
extract() {
    grep -aoE "$PATTERN" "$LOG" 2>/dev/null | tr -d '\r' \
        | head -n "$WANT" > "$1.raw" || true
    if [ "$GOLDEN_SORTED" = "1" ]; then
        LC_ALL=C sort "$1.raw" > "$1"
    else
        cat "$1.raw" > "$1"
    fi
}

mkdir -p "$WORK" "$FOLDER"
LOG="$WORK/serial.log"
rm -f "$LOG"

# The drive is attached READ-ONLY, matching the default guest build,
# whose driver refuses writes (CONF_WITH_VIRTIO_BLK_WRITE is 0).
#
# readonly=on is required for that, not a precaution: without it QEMU
# refuses to start at all with "Block node is read-only", because a
# vvfat drive opened without "rw:" is a read-only block node and
# virtio-blk asks for write permission unless told otherwise.
#
# SHINOGI_VVFAT_RW=1 attaches it read-write instead, for a guest built
# with CONF_WITH_VIRTIO_BLK_WRITE=1. That takes two separate changes on
# this side and both are needed: the "rw:" prefix puts vvfat itself into
# read-write mode, and readonly=on has to go so the block node grants
# the write permission. THE GUEST THEN WRITES TO THIS FOLDER, and vvfat
# in that mode loses host data -- see tools/check-vvfat-write.py, which
# is what actually measures it. Point it only at a folder
# tools/make-fixtures.py can rebuild.
if [ -n "$VVFAT" ]; then
    [ -d "$VVFAT" ] || { echo "no vvfat folder at $VVFAT" >&2; exit 2; }
    if [ "${SHINOGI_VVFAT_RW:-0}" = "1" ]; then
        DRIVE="file=fat:rw:$VVFAT,format=raw,if=none,id=hostblk"
    else
        DRIVE="file=fat:$VVFAT,format=raw,if=none,id=hostblk,readonly=on"
    fi
    set -- \
        -drive "$DRIVE" \
        -device virtio-blk-device,drive=hostblk
else
    set --
fi

qemu-system-m68k \
    -M virt -m 128 \
    -kernel "$ELF" \
    -device virtio-gpu-device \
    -fsdev "local,id=hostfs,path=$FOLDER,security_model=mapped-xattr" \
    -device virtio-9p-device,fsdev=hostfs,mount_tag=shinogi \
    "$@" \
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
        extract "$WORK/probe"
        if cmp -s "$EXPECT" "$WORK/probe"; then
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
#
# Only the FIRST $WANT matching lines are compared. The guest keeps
# running after it has produced them -- the desktop re-lists the drive,
# so these lines repeat for as long as the capture window is open -- and
# comparing everything would make each golden depend on how fast the
# host booted it. That is not hypothetical: it passed locally and failed
# on a CI runner purely because the window stayed open longer.
#
# The trade is real: a spurious line appearing AFTER the expected ones
# is not caught. A wrong line, a missing line, or a wrong order still
# is, and those are the failures these goldens exist to find.
#
# (Under GOLDEN_SORTED=1 the order claim is given up on purpose -- see
# the note at the top of this file. Everything else still holds.)
extract "$WORK/actual"

if diff -u "$EXPECT" "$WORK/actual"; then
    echo "PASS $NAME"
    exit 0
fi

echo "FAIL $NAME - see $LOG" >&2
exit 1

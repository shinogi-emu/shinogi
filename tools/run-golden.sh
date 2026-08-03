#!/bin/sh
#
# Boot the guest and check its serial output against a golden file.
#
# Usage: tools/run-golden.sh <name> <grep-ere> [host-folder]
#
#   <name>      tests/golden/<name>.expected holds the expected lines
#   <grep-ere>  extended regex selecting the lines to compare
#
# Exits 0 only when the extracted lines match the golden file exactly,
# in order. Any mismatch prints a diff and exits 1.
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

# The guest never exits on its own; give it a fixed window then stop it.
i=0
while [ "$i" -lt "$BOOT_WAIT" ]; do
    sleep 1
    i=$((i + 1))
done
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true

grep -aoE "$PATTERN" "$LOG" > "$WORK/actual" || true

if diff -u "$GOLDEN" "$WORK/actual"; then
    echo "PASS $NAME"
    exit 0
fi

echo "FAIL $NAME - see $LOG" >&2
exit 1

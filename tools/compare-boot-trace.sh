#!/bin/sh
#
# Compare a boot trace against the golden one.
#
# Addresses churn on every rebuild -- any change to code size moves the
# whole memory map -- so comparing raw traces produces noise that hides
# the thing worth seeing. What matters is the SEQUENCE of initialisation
# steps: if a later change perturbs init order, it shows up here as a
# diff rather than as a mystery three phases downstream.
#
# Usage: tools/compare-boot-trace.sh <new-trace> [golden]
#
set -eu

NEW="$1"
GOLDEN="${2:-tests/golden/boot-phase2.log}"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

normalise() {
    # Mask hex addresses and decimal sizes, drop QEMU's own exit line.
    sed -E \
        -e 's/0x[0-9a-fA-F]{4,8}/0xADDR/g' \
        -e 's/=[0-9]{4,}/=NUM/g' \
        -e '/terminating on signal/d' \
        "$1" > "$2"
}

normalise "$GOLDEN" "$TMP/golden"
normalise "$NEW" "$TMP/new"

if diff -u "$TMP/golden" "$TMP/new"; then
    echo "boot trace matches golden (init sequence unchanged)"
else
    echo
    echo "boot trace DIFFERS from golden -- init order or steps changed."
    echo "If the change is intended, refresh with:"
    echo "  cp $NEW $GOLDEN"
    exit 1
fi

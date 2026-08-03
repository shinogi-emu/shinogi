#!/bin/sh
#
# Run every golden test, in one command.
#
# The pairing of a golden file with the regex that extracts its lines
# from the serial log lived nowhere until now: run-golden.sh takes the
# pattern as an argument, so each golden's pattern was retyped from
# memory on every verification pass and a mistyped one silently compares
# the wrong lines -- usually none of them, which looks like a pass on an
# empty extraction only because run-golden.sh refuses an empty golden.
# The table below is the single record of which pattern belongs to which
# golden.
#
# Usage: tools/run-all-goldens.sh [name ...]
#
#   With no arguments, every golden runs. With names, only those.
#
# Exit codes:
#   0 = every golden selected ran and matched
#   1 = at least one golden ran and did not match
#   2 = at least one test could not be run at all (harness failure);
#       this is never reported as a pass or a fail
#
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
FOLDER="${SHINOGI_HOSTFS:-/tmp/shinogi-hostfs}"

# name<TAB>pattern. Keep in step with the comment at the top of
# tools/make-fixtures.py, which records the same pairing.
GOLDENS="
phase5-attach	9p: (slot [0-9]+ msize|attached,).*
phase5-walk	9p: (walk|getattr).*
phase5-readdir	9p: dirent.*
phase5-drive	hostfs: drive C registered
phase5-dates	hostfs: date FIXED.TXT.*
phase5-read	hostfs: (open|read|seek|close|wrap) .*
phase5-bigread	hostfs: big .*
phase5-paths	hostfs: (rwopen|rwread|rwwrite|rwclose|setpath|getpath|dfree|escape|fattrib|setdrv|specname|longname|dtaguard).*
phase5-subdir	hostfs: sub.*
phase5-listing	hostfs: fs(first|next).*
phase5-dta	hostfs: dta.*
phase5-many	hostfs: many.*
"

want=""
if [ "$#" -gt 0 ]; then
    want=" $* "
fi

# phase5-bigread asserts an FNV-1a hash the guest computes over BIG.DAT.
# A hash is the one golden line that cannot be read and judged by eye, so
# it is checked here against the same hash computed from the fixture
# definition -- an independent calculation, not a re-recording of
# whatever the guest last printed.
python3 - "$ROOT" <<'EOF' || exit 2
import sys, os
sys.dont_write_bytecode = True      # no __pycache__ in the source tree
import importlib.util
spec = importlib.util.spec_from_file_location(
    "mkfix", os.path.join(sys.argv[1], "tools", "make-fixtures.py"))
mkfix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mkfix)

data = mkfix.bigread_bytes()
h = 2166136261
for b in data:
    h ^= b
    h = (h * 16777619) & 0xffffffff

want = "hostfs: big %d bytes fnv1a 0x%08x" % (len(data), h)
path = os.path.join(sys.argv[1], "tests", "golden", "phase5-bigread.expected")
lines = open(path).read().splitlines()
if want not in lines:
    sys.stderr.write("phase5-bigread golden does not match the fixture:\n"
                     "  expected: %s\n  golden:   %s\n" % (want, lines))
    sys.exit(1)
EOF

pass=0
fail=0
error=0
failed_names=""

echo "$GOLDENS" | while IFS='	' read -r name pattern; do
    [ -n "$name" ] || continue
    printf '%s\t%s\n' "$name" "$pattern"
done > /tmp/.run-all-goldens.$$

while IFS='	' read -r name pattern; do
    [ -n "$name" ] || continue
    if [ -n "$want" ]; then
        case "$want" in
            *" $name "*) ;;
            *) continue ;;
        esac
    fi

    "$ROOT/tools/run-golden.sh" "$name" "$pattern" "$FOLDER"
    case $? in
        0) pass=$((pass + 1)) ;;
        1) fail=$((fail + 1)); failed_names="$failed_names $name" ;;
        *) error=$((error + 1)); failed_names="$failed_names $name(harness)" ;;
    esac
done < /tmp/.run-all-goldens.$$

python3 -c "import os,sys; os.remove(sys.argv[1])" "/tmp/.run-all-goldens.$$"

echo
echo "goldens: $pass passed, $fail failed, $error could not run"
[ -z "$failed_names" ] || echo "not green:$failed_names"

[ "$error" -eq 0 ] || exit 2
[ "$fail" -eq 0 ] || exit 1
exit 0

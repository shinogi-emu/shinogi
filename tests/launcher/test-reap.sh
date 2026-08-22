#!/bin/sh
#
# The drive C helper must not outlive the guest.
#
# Closing the SDL window makes QEMU exit on its own, so "shinogi stop"
# never runs and shinogi-hostfsd is left listening on its socket - status
# says "stopped" while a helper still holds drive C, and the next start
# orphans another one. reap_if_gone() in the launcher is what clears that
# up; this checks that it still does.
#
# A dead QEMU is faked with a pid that has already exited and the helper
# with a sleep, so nothing here needs a built bundle or a display. Revert
# reap_if_gone and both cases fail - that is what makes this a test.
#
# Usage: tests/launcher/test-reap.sh [path/to/shinogi.sh]
#
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC="${1:-$ROOT/tools/linux/shinogi.sh}"
[ -f "$SRC" ] || { echo "no launcher at $SRC" >&2; exit 2; }

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
rc=0

# The launcher is a template; the bundle substitutes these at packaging
# time and an unsubstituted @CPU@ would make every case below fail for
# the wrong reason.
mkdir -p "$T/pkg"
sed -e 's|@CPU@|m68040|g' -e 's|@EDITION@|test|g' -e 's|@VERSION@|0.0.0|g' \
    "$SRC" > "$T/pkg/shinogi"
chmod +x "$T/pkg/shinogi"
if grep -q '@CPU@\|@EDITION@\|@VERSION@' "$T/pkg/shinogi"; then
    echo "FAIL: the launcher still has unsubstituted placeholders" >&2
    exit 2
fi

# What the close box leaves behind: a helper still running, and a pid file
# naming a QEMU that has gone.
leave_orphan() {
    home="$1"
    mkdir -p "$home/run" "$home/logs"
    sleep 300 &
    HELPER=$!
    echo "$HELPER" > "$home/run/hostfsd.pid"
    sh -c 'exit 0' & gone=$!; wait "$gone" 2>/dev/null
    echo "$gone" > "$home/run/qemu.pid"
    touch "$home/run/hostfsd.ready"
    kill -0 "$HELPER" 2>/dev/null || { echo "the fake helper never ran" >&2; exit 2; }
}

check_reaped() {
    what="$1"; home="$2"
    sleep 0.3
    if kill -0 "$HELPER" 2>/dev/null; then
        echo "FAIL: $what left the drive C helper (pid $HELPER) running"
        kill "$HELPER" 2>/dev/null
        rc=1
    elif [ -f "$home/run/qemu.pid" ] || [ -f "$home/run/hostfsd.pid" ]; then
        echo "FAIL: $what left a stale pid file behind"
        rc=1
    else
        echo "ok: $what reaped the helper"
    fi
}

# status: reports the truth, and leaves nothing behind while doing it.
leave_orphan "$T/status"
out=$(SHINOGI_HOME="$T/status" HOME="$T" "$T/pkg/shinogi" status 2>&1)
case "$out" in
    stopped*) ;;
    *) echo "FAIL: status did not report stopped:"; echo "$out"; rc=1 ;;
esac
check_reaped "status" "$T/status"

# start: reaps BEFORE it starts a new helper, or the old one is orphaned
# for good - start_hostfsd overwrites the pid file that named it. This
# start cannot get as far as QEMU (there is no bundle here) and is not
# meant to; the reap has to have happened before it gives up.
leave_orphan "$T/start"
mkdir -p "$T/pkg/guest/drive-c"
SHINOGI_HOME="$T/start" HOME="$T" "$T/pkg/shinogi" start --display none >/dev/null 2>&1
check_reaped "start" "$T/start"

[ "$rc" -eq 0 ] && echo "PASS"
exit "$rc"

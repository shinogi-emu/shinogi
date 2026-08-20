#!/bin/sh
#
# shinogi - run the guest on Linux, with or without a display.
#
# This is the launcher inside the self-contained Linux bundle. Everything
# it needs is beside it: the patched QEMU, that QEMU's whole library
# closure down to and including the C library and the dynamic loader, the
# guest image, and a pristine drive C.
#
# WHY THE LOADER IS BUNDLED. The QEMU here is built on a current Ubuntu
# and binds cfsetispeed/cfsetospeed at GLIBC_2.42 and sqrtf at
# GLIBC_2.43. No shipping WSL image has those, and the failure is a
# refusal to start with "version `GLIBC_2.43' not found" -- which reads
# like a broken download rather than a distribution mismatch. Running the
# binary through the loader that was bundled with it removes the question
# entirely. The system loader is still tried first, so a host new enough
# to run it directly does.
#
# Usage: shinogi <command> [options]
# Run "shinogi help" for the list.
#
set -eu

PKG=$(cd "$(dirname "$0")" && pwd)
BIN="$PKG/bin"
LIB="$PKG/lib"
GUEST="$PKG/guest"

# Runtime state - drive C, logs, the pid file and the monitor socket -
# lives outside the bundle so the bundle stays read-only and a second
# copy of it does not fight over the same files.
HOME_DIR="${SHINOGI_HOME:-$HOME/.shinogi}"
DRIVE_C="${SHINOGI_DRIVE_C:-$HOME_DIR/drive-c}"
RUNDIR="$HOME_DIR/run"
PIDFILE="$RUNDIR/qemu.pid"

# The monitor socket. AF_UNIX paths are capped at 108 bytes and QEMU
# refuses to start rather than truncating, so a SHINOGI_HOME deep in a
# temporary directory would break the launcher for a reason nothing in
# the message connects to SHINOGI_HOME. Fall back to a short path under
# the user's runtime directory when the natural one will not fit.
MONSOCK="$RUNDIR/monitor.sock"
QMPSOCK="$RUNDIR/qmp.sock"
HOSTFS_SOCK="$RUNDIR/hostfs.sock"
if [ ${#MONSOCK} -ge 100 ] || [ ${#HOSTFS_SOCK} -ge 100 ]; then
    MONDIR="${XDG_RUNTIME_DIR:-/tmp}/shinogi-$(id -u)"
    mkdir -p "$MONDIR" 2>/dev/null || MONDIR=/tmp
    TAG=$(printf '%s' "$RUNDIR" | cksum | cut -d' ' -f1)
    MONSOCK="$MONDIR/$TAG-mon.sock"
    QMPSOCK="$MONDIR/$TAG-qmp.sock"
    HOSTFS_SOCK="$MONDIR/$TAG-hostfs.sock"
fi
HOSTFS_PID="$RUNDIR/hostfsd.pid"
HOSTFS_READY="$RUNDIR/hostfsd.ready"
HOSTFS_LOG="$HOME_DIR/logs/hostfsd.log"
SERIAL="$HOME_DIR/logs/serial.log"
ERRLOG="$HOME_DIR/logs/guest-errors.log"
QEMULOG="$HOME_DIR/logs/qemu.log"

CPU="${SHINOGI_CPU:-@CPU@}"
EDITION="@EDITION@"
VERSION="@VERSION@"
XRES="${SHINOGI_XRES:-1280}"
YRES="${SHINOGI_YRES:-720}"
MEM="${SHINOGI_MEM:-128}"

die() { echo "shinogi: $*" >&2; exit 1; }

# ---------------------------------------------------------------- qemu

# Set QEMU_PREFIX to the argv prefix that starts the bundled QEMU. The
# direct form is tried first and only accepted if it actually runs, so
# this is a probe rather than a guess about the host's glibc.
resolve_qemu() {
    [ -x "$BIN/qemu-system-m68k" ] || die "bundle is incomplete: no $BIN/qemu-system-m68k"
    # SHINOGI_LOADER pins the choice, which is how the bundled path gets
    # tested on a host new enough not to need it.
    case "${SHINOGI_LOADER:-auto}" in
        native)  QEMU_MODE=native; return 0 ;;
        bundled) [ -x "$LIB/ld-linux-x86-64.so.2" ] ||
                     die "SHINOGI_LOADER=bundled but the bundle has no loader"
                 QEMU_MODE=bundled; return 0 ;;
        auto) ;;
        *) die "SHINOGI_LOADER must be auto, native or bundled" ;;
    esac
    if LD_LIBRARY_PATH="$LIB" "$BIN/qemu-system-m68k" --version >/dev/null 2>&1; then
        QEMU_MODE=native
    elif [ -x "$LIB/ld-linux-x86-64.so.2" ] &&
         "$LIB/ld-linux-x86-64.so.2" --library-path "$LIB" \
             "$BIN/qemu-system-m68k" --version >/dev/null 2>&1; then
        QEMU_MODE=bundled
    else
        echo "shinogi: the bundled QEMU will not start on this host." >&2
        echo "diagnostic:" >&2
        LD_LIBRARY_PATH="$LIB" "$BIN/qemu-system-m68k" --version >&2 || true
        exit 1
    fi
}

run_qemu() {
    resolve_qemu
    if [ "$QEMU_MODE" = native ]; then
        LD_LIBRARY_PATH="$LIB" exec "$BIN/qemu-system-m68k" "$@"
    else
        exec "$LIB/ld-linux-x86-64.so.2" --library-path "$LIB" \
             "$BIN/qemu-system-m68k" "$@"
    fi
}

run_in_foreground() {
    resolve_qemu
    if [ "$QEMU_MODE" = native ]; then
        LD_LIBRARY_PATH="$LIB" "$BIN/qemu-system-m68k" "$@"
    else
        "$LIB/ld-linux-x86-64.so.2" --library-path "$LIB" \
            "$BIN/qemu-system-m68k" "$@"
    fi
}

# ------------------------------------------------------------ drive C

# The bundle ships drive C pristine and this copies it out on first use,
# so a guest that corrupts its own C: is one "shinogi reset-drive-c"
# away from working again, and the copy the user edits is never inside
# the bundle they might replace with the next build.
ensure_drive_c() {
    [ -d "$DRIVE_C" ] && return 0
    [ -d "$GUEST/drive-c" ] || die "bundle is incomplete: no $GUEST/drive-c"
    echo "shinogi: creating drive C at $DRIVE_C"
    mkdir -p "$(dirname "$DRIVE_C")"
    cp -a "$GUEST/drive-c" "$DRIVE_C"
}

# -------------------------------------------------------------- hostfsd

# QEMU is the CLIENT on this socket (server=off), so the helper has to be
# listening before QEMU parses its command line. --ready-file is what
# makes that a wait rather than a sleep: the file appears only once the
# listener is accepting.
start_hostfsd() {
    [ -x "$BIN/shinogi-hostfsd" ] || die "bundle is incomplete: no $BIN/shinogi-hostfsd"
    rm -f "$HOSTFS_SOCK" "$HOSTFS_READY"
    "$BIN/shinogi-hostfsd" --root "$DRIVE_C" --listen "$HOSTFS_SOCK" \
        --ready-file "$HOSTFS_READY" >"$HOSTFS_LOG" 2>&1 &
    echo $! > "$HOSTFS_PID"
    i=0
    while [ $i -lt 100 ]; do
        [ -e "$HOSTFS_READY" ] && return 0
        kill -0 "$(cat "$HOSTFS_PID")" 2>/dev/null || {
            echo "shinogi: the drive C helper exited immediately" >&2
            cat "$HOSTFS_LOG" >&2
            rm -f "$HOSTFS_PID"
            exit 1
        }
        i=$((i + 1))
        sleep 0.1 2>/dev/null || sleep 1
    done
    die "the drive C helper never came up (see $HOSTFS_LOG)"
}

stop_hostfsd() {
    [ -f "$HOSTFS_PID" ] || return 0
    hpid=$(cat "$HOSTFS_PID" 2>/dev/null) || hpid=
    [ -n "$hpid" ] && kill "$hpid" 2>/dev/null || true
    rm -f "$HOSTFS_PID" "$HOSTFS_READY" "$HOSTFS_SOCK"
}

# --------------------------------------------------------------- state

running() {
    [ -f "$PIDFILE" ] || return 1
    pid=$(cat "$PIDFILE" 2>/dev/null) || return 1
    [ -n "$pid" ] || return 1
    kill -0 "$pid" 2>/dev/null
}

require_running() {
    running || die "no guest is running (start one with: shinogi start)"
}

mon() {
    require_running
    "$BIN/shinogi-monitor" "$MONSOCK" "$@"
}

qmp() {
    require_running
    "$BIN/shinogi-monitor" --qmp "$QMPSOCK" "$1"
}

# Several QMP commands down one connection, paced. The gap matters: the
# guest samples the mouse once a frame, so anything faster than a frame
# is a transition it never sees.
qmp_seq() {
    require_running
    gap=$1
    shift
    "$BIN/shinogi-monitor" --qmp-seq "$QMPSOCK" "$gap" "$@"
}

CLICK_MS="${SHINOGI_CLICK_MS:-40}"

# ------------------------------------------------------------ the pointer

# The guest's pointing device is a tablet: it reports where the pointer
# IS, not how far it moved, and the axis range it speaks is a fixed
# 0..32767 regardless of the resolution. So a pixel coordinate has to be
# scaled into that range, and the caller never sees it.
#
# Integer arithmetic throughout, with the +/2 rounding that keeps
# "move to x" and "read the pointer back" agreeing at the edges.
abs_axis() {
    pixel=$1
    span=$2
    [ "$span" -gt 1 ] || span=2
    echo $(( (pixel * 32767 + (span - 1) / 2) / (span - 1) ))
}

# The JSON for one input-send-event, printed rather than sent, so a
# caller can hand several of them to qmp_seq in one go.
move_event() {
    ax=$(abs_axis "$1" "$XRES")
    ay=$(abs_axis "$2" "$YRES")
    printf '%s' "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[\
{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$ax}},\
{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$ay}}]}}"
}

button_event() {
    printf '%s' "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[\
{\"type\":\"btn\",\"data\":{\"down\":$2,\"button\":\"$1\"}}]}}"
}

pointer_to() {
    qmp "$(move_event "$1" "$2")" >/dev/null
}

check_xy() {
    case "$1$2" in
        *[!0-9]*|"") die "coordinates must be whole numbers of pixels" ;;
    esac
    [ "$1" -lt "$XRES" ] && [ "$2" -lt "$YRES" ] ||
        die "($1,$2) is outside the ${XRES}x${YRES} screen"
}

# ------------------------------------------------------------ commands

# ORDER ON THE COMMAND LINE IS LOAD-BEARING. QEMU fills the virtio-mmio
# transport slots from the top down, so the first device listed lands in
# the highest slot and the golden tests pin a slot by number. Keep the
# order identical to the Windows launcher: net, gpu, keyboard, tablet,
# then the drive C transport last.
#
# Drive C is served by shinogi-hostfsd over a virtio-serial port, which
# is the same arrangement the Windows bundle uses. QEMU's own 9p backend
# is available on Linux and was tried first; it hands the guest whatever
# order the host filesystem enumerates in, and the AUTO folder is
# order-sensitive -- with 060SP.PRG landing after FVDI.PRG the guest
# panics on an unimplemented integer instruction before it reaches the
# desktop. Windows gets away with it because NTFS enumerates in name
# order. hostfsd sorts, so both platforms see the same order, and the
# order they see is the one the AUTO folder needs.
qemu_args() {
    printf '%s\n' \
        -name "$EDITION $VERSION" \
        -M virt \
        -L "$PKG/share/qemu" \
        -cpu "$CPU" \
        -m "$MEM" \
        -kernel "$GUEST/emutos-virt.elf" \
        -netdev user,id=net0,ipv6=off \
        -device virtio-net-device,netdev=net0 \
        -device "virtio-gpu-device,xres=$XRES,yres=$YRES" \
        -device virtio-keyboard-device \
        -device virtio-tablet-device \
        -chardev "socket,id=hostfs,path=$HOSTFS_SOCK,server=off" \
        -device virtio-serial-device \
        -device virtserialport,chardev=hostfs,name=shinogi.hostfs \
        -serial "file:$SERIAL" \
        -monitor "unix:$MONSOCK,server,nowait" \
        -qmp "unix:$QMPSOCK,server=on,wait=off" \
        -d guest_errors -D "$ERRLOG"
}

cmd_start() {
    display=none
    while [ $# -gt 0 ]; do
        case "$1" in
            --display) display="${2:?--display needs a value}"; shift 2 ;;
            --display=*) display="${1#--display=}"; shift ;;
            --cpu) CPU="${2:?--cpu needs a value}"; shift 2 ;;
            --cpu=*) CPU="${1#--cpu=}"; shift ;;
            *) die "unknown option to start: $1" ;;
        esac
    done

    running && die "a guest is already running (pid $(cat "$PIDFILE"))"

    ensure_drive_c
    mkdir -p "$RUNDIR" "$HOME_DIR/logs"
    # A stale socket makes QEMU fail to bind and the failure is reported
    # as an unrelated chardev error, so clear it before every start.
    rm -f "$MONSOCK" "$QMPSOCK"
    : > "$SERIAL"
    : > "$ERRLOG"
    start_hostfsd

    # The argument list is newline-separated because paths may contain
    # spaces; xargs -d '\n' would need GNU xargs, so the shell reads it.
    (
        set --
        while IFS= read -r a; do set -- "$@" "$a"; done <<EOF
$(qemu_args)
EOF
        exec >"$QEMULOG" 2>&1
        run_qemu "$@" -display "$display"
    ) &
    echo $! > "$PIDFILE"

    # QEMU dying on its command line is the common failure and it dies
    # fast, so give it a moment and report rather than returning a
    # success the caller will only discover was false later.
    i=0
    while [ $i -lt 40 ]; do
        [ -S "$MONSOCK" ] && break
        running || { echo "shinogi: QEMU exited immediately" >&2
                     cat "$QEMULOG" >&2; rm -f "$PIDFILE"; stop_hostfsd; exit 1; }
        i=$((i + 1))
        sleep 0.1 2>/dev/null || sleep 1
    done

    echo "shinogi: started ($EDITION $VERSION, $CPU, display $display) pid $(cat "$PIDFILE")"
    echo "  drive C: $DRIVE_C"
    echo "  serial:  $SERIAL"
    echo "the guest takes roughly a minute to reach the desktop."
}

cmd_run() {
    display="${SHINOGI_DISPLAY:-sdl}"
    while [ $# -gt 0 ]; do
        case "$1" in
            --display) display="${2:?--display needs a value}"; shift 2 ;;
            --display=*) display="${1#--display=}"; shift ;;
            --cpu) CPU="${2:?--cpu needs a value}"; shift 2 ;;
            --cpu=*) CPU="${1#--cpu=}"; shift ;;
            *) die "unknown option to run: $1" ;;
        esac
    done
    ensure_drive_c
    mkdir -p "$RUNDIR" "$HOME_DIR/logs"
    rm -f "$MONSOCK" "$QMPSOCK"
    start_hostfsd
    trap 'stop_hostfsd' EXIT INT TERM
    set --
    while IFS= read -r a; do set -- "$@" "$a"; done <<EOF
$(qemu_args)
EOF
    # No exec here: the trap has to survive QEMU so the helper is not
    # left listening after the window closes.
    run_in_foreground "$@" -display "$display"
    stop_hostfsd
}

cmd_stop() {
    running || { stop_hostfsd; echo "shinogi: nothing running"; return 0; }
    pid=$(cat "$PIDFILE")
    # quit through the monitor first: it lets QEMU close the 9p backend,
    # so drive C is not left with a half-written file.
    "$BIN/shinogi-monitor" "$MONSOCK" quit >/dev/null 2>&1 || kill "$pid" 2>/dev/null || true
    i=0
    while [ $i -lt 50 ] && kill -0 "$pid" 2>/dev/null; do
        i=$((i + 1))
        sleep 0.1 2>/dev/null || sleep 1
    done
    kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null || true
    rm -f "$PIDFILE" "$MONSOCK" "$QMPSOCK"
    stop_hostfsd
    echo "shinogi: stopped"
}

cmd_status() {
    if running; then
        echo "running   pid $(cat "$PIDFILE")"
    else
        echo "stopped"
    fi
    echo "edition   $EDITION $VERSION ($CPU)"
    echo "drive C   $DRIVE_C"
    echo "serial    $SERIAL"
    echo "monitor   $MONSOCK"
    if [ -f "$SERIAL" ]; then
        echo "serial log is $(wc -c < "$SERIAL") bytes; last line:"
        tail -n 1 "$SERIAL" 2>/dev/null || true
    fi
}

cmd_log() {
    [ -f "$SERIAL" ] || die "no serial log yet at $SERIAL"
    case "${1:-}" in
        -f) exec tail -f "$SERIAL" ;;
        "") exec tail -n 40 "$SERIAL" ;;
        *) exec tail -n "$1" "$SERIAL" ;;
    esac
}

# Capture, optionally cutting out a region and magnifying it.
#
# Without --crop or --scale the emulator writes the PNG itself. With
# either, it writes a PPM and shinogi-shot does the rest, because the
# emulator can only give the whole screen at its own size and the whole
# screen at its own size is the wrong picture for most questions: a GEM
# dialog is a few hundred pixels inside 1280x720.
cmd_screenshot() {
    out=
    crop=
    scale=
    while [ $# -gt 0 ]; do
        case "$1" in
            --crop) crop="${2:?--crop needs X,Y,W,H}"; shift 2 ;;
            --crop=*) crop="${1#--crop=}"; shift ;;
            --scale) scale="${2:?--scale needs a number}"; shift 2 ;;
            --scale=*) scale="${1#--scale=}"; shift ;;
            -*) die "unknown option to screenshot: $1" ;;
            *) [ -n "$out" ] && die "screenshot takes one filename"
               out="$1"; shift ;;
        esac
    done
    out="${out:-$HOME_DIR/logs/screen.png}"
    case "$out" in
        /*) ;;
        *) out="$PWD/$out" ;;
    esac

    if [ -z "$crop" ] && [ -z "$scale" ]; then
        fmt=png
        case "$out" in *.ppm) fmt=ppm ;; esac
        mon screendump "$out" -f "$fmt" >/dev/null
        # screendump answers with nothing on success, so the file is the
        # only evidence there is - check it rather than the exit status.
        [ -s "$out" ] || die "screendump produced nothing at $out"
        echo "$out"
        return 0
    fi

    raw="$RUNDIR/shot.ppm"
    mon screendump "$raw" -f ppm >/dev/null
    [ -s "$raw" ] || die "screendump produced nothing"
    set --
    [ -n "$crop" ] && set -- "$@" --crop "$crop"
    [ -n "$scale" ] && set -- "$@" --scale "$scale"
    size=$("$BIN/shinogi-shot" "$raw" "$out" "$@")
    rm -f "$raw"
    echo "$out ($size)"
}

# Wait for the screen to stop changing.
#
# The alternative is guessing a sleep, and every guess is either too
# short -- so the screenshot catches a half-drawn window and the caller
# reads it as a rendering fault -- or too long. Two identical captures in
# a row mean the guest has finished drawing whatever it was drawing.
cmd_wait_idle() {
    limit="${1:-30}"
    a="$RUNDIR/idle-a.ppm"
    b="$RUNDIR/idle-b.ppm"
    rm -f "$a" "$b"
    mon screendump "$a" -f ppm >/dev/null
    i=0
    while [ "$i" -lt "$limit" ]; do
        sleep 1
        mon screendump "$b" -f ppm >/dev/null
        if cmp -s "$a" "$b"; then
            rm -f "$a" "$b"
            echo "settled after ${i}s"
            return 0
        fi
        mv "$b" "$a"
        i=$((i + 1))
    done
    rm -f "$a" "$b"
    echo "still changing after ${limit}s" >&2
    return 1
}

# GEM is a keyboard-and-mouse system with no other way in, so a caller
# with no display needs these to get past a dialog.
cmd_key() {
    [ $# -gt 0 ] || die "usage: shinogi key <keyname> [keyname...]"
    for k in "$@"; do
        mon sendkey "$k" >/dev/null
    done
}

# Only the characters a QEMU keymap names. Anything outside that set is
# reported rather than silently dropped, because a password or a path
# that lost a character is worse than one that was refused.
char_key() {
    case "$1" in
        [a-z0-9]) printf '%s' "$1" ;;
        [A-Z]) printf 'shift-%s' "$(printf '%s' "$1" | tr 'A-Z' 'a-z')" ;;
        ' ') printf 'spc' ;;
        '.') printf 'dot' ;;
        ',') printf 'comma' ;;
        '-') printf 'minus' ;;
        '=') printf 'equal' ;;
        '/') printf 'slash' ;;
        '\') printf 'backslash' ;;
        ';') printf 'semicolon' ;;
        "'") printf 'apostrophe' ;;
        '[') printf 'bracket_left' ;;
        ']') printf 'bracket_right' ;;
        ':') printf 'shift-semicolon' ;;
        '_') printf 'shift-minus' ;;
        '?') printf 'shift-slash' ;;
        '*') printf 'shift-8' ;;
        '(') printf 'shift-9' ;;
        ')') printf 'shift-0' ;;
        *) return 1 ;;
    esac
}

cmd_type() {
    [ $# -gt 0 ] || die "usage: shinogi type <text>"
    text="$*"
    i=1
    len=${#text}
    while [ "$i" -le "$len" ]; do
        c=$(printf '%s' "$text" | cut -c "$i")
        if ! k=$(char_key "$c"); then
            die "no key name for character '$c' at position $i"
        fi
        mon sendkey "$k" >/dev/null
        i=$((i + 1))
    done
}

cmd_move() {
    [ $# -eq 2 ] || die "usage: shinogi move <x> <y>"
    check_xy "$1" "$2"
    pointer_to "$1" "$2"
}

# A click optionally moves first, because "click at" is what a caller
# almost always means and doing it in two commands leaves a window in
# which something else could move the pointer.
#
# The press and the release are a frame apart. Sending them together
# looks like it works -- QMP accepts both and reports success -- and the
# guest never sees a button go down.
cmd_click() {
    btn=left
    case "${1:-}" in
        --right) btn=right; shift ;;
        --middle) btn=middle; shift ;;
    esac
    if [ $# -eq 2 ]; then
        check_xy "$1" "$2"
        qmp_seq "$CLICK_MS" "$(move_event "$1" "$2")" \
                "$(button_event "$btn" true)" \
                "$(button_event "$btn" false)" >/dev/null
    elif [ $# -eq 0 ]; then
        qmp_seq "$CLICK_MS" "$(button_event "$btn" true)" \
                            "$(button_event "$btn" false)" >/dev/null
    else
        die "usage: shinogi click [--right|--middle] [<x> <y>]"
    fi
}

# Two clicks close enough together to count as one gesture, and far
# enough apart to be seen at all. SHINOGI_CLICK_MS moves both bounds if a
# guest turns out to want something else.
cmd_dclick() {
    if [ $# -eq 2 ]; then
        check_xy "$1" "$2"
        qmp_seq "$CLICK_MS" "$(move_event "$1" "$2")" \
                "$(button_event left true)" "$(button_event left false)" \
                "$(button_event left true)" "$(button_event left false)" \
                >/dev/null
    elif [ $# -eq 0 ]; then
        qmp_seq "$CLICK_MS" \
                "$(button_event left true)" "$(button_event left false)" \
                "$(button_event left true)" "$(button_event left false)" \
                >/dev/null
    else
        die "usage: shinogi dclick [<x> <y>]"
    fi
}

# Dragging needs the motion in between, not just the endpoints: a window
# being moved follows the pointer, and a single jump from start to finish
# gives it nothing to follow.
cmd_drag() {
    [ $# -eq 4 ] || die "usage: shinogi drag <x1> <y1> <x2> <y2>"
    check_xy "$1" "$2"
    check_xy "$3" "$4"
    steps="${SHINOGI_DRAG_STEPS:-12}"
    set -- "$1" "$2" "$3" "$4" \
           "$(move_event "$1" "$2")" "$(button_event left true)"
    i=1
    while [ "$i" -le "$steps" ]; do
        set -- "$@" "$(move_event $(( $1 + ($3 - $1) * i / steps )) \
                                  $(( $2 + ($4 - $2) * i / steps )))"
        i=$((i + 1))
    done
    set -- "$@" "$(button_event left false)"
    shift 4
    qmp_seq "$CLICK_MS" "$@" >/dev/null
}

cmd_reset_drive_c() {
    [ -d "$GUEST/drive-c" ] || die "bundle is incomplete: no $GUEST/drive-c"
    running && die "stop the guest first (shinogi stop)"
    if [ -d "$DRIVE_C" ]; then
        backup="$DRIVE_C.old"
        [ -e "$backup" ] && die "$backup already exists - move it aside first"
        mv "$DRIVE_C" "$backup"
        echo "shinogi: previous drive C kept at $backup"
    fi
    cp -a "$GUEST/drive-c" "$DRIVE_C"
    echo "shinogi: drive C restored to the shipped tree at $DRIVE_C"
}

cmd_doctor() {
    echo "$EDITION $VERSION"
    echo "bundle    $PKG"
    resolve_qemu
    echo "qemu      $QEMU_MODE"
    if [ "$QEMU_MODE" = bundled ]; then
        echo "          (host glibc is older than this build; using the bundled loader)"
    fi
    LD_LIBRARY_PATH="$LIB" "$BIN/qemu-system-m68k" --version 2>/dev/null | head -1 ||
      "$LIB/ld-linux-x86-64.so.2" --library-path "$LIB" "$BIN/qemu-system-m68k" --version | head -1
    echo "guest     $GUEST/emutos-virt.elf ($(wc -c < "$GUEST/emutos-virt.elf") bytes)"
    echo "drive C   $DRIVE_C $( [ -d "$DRIVE_C" ] && echo "(present)" || echo "(created on first start)" )"
    if [ -r /proc/version ] && grep -qi microsoft /proc/version; then
        echo "host      WSL detected"
        echo "          headless works as-is.  For a window, WSLg is an RDP-backed"
        echo "          compositor and SDL's pointer grab does not release there:"
        echo "          use  shinogi run --display gtk"
    fi
}

cmd_help() {
    cat <<EOF
$EDITION $VERSION - Atari machine emulator, Linux bundle

  shinogi start [--display none|sdl|gtk] [--cpu m68040|m68060]
                          boot in the background, no display by default
  shinogi run [--display sdl|gtk]
                          boot in the foreground with a window
  shinogi stop            shut the running guest down
  shinogi status          is it running, and where its files are
  shinogi log [-f|N]      the guest's serial console
  shinogi screenshot [file.png] [--crop X,Y,W,H] [--scale N]
                          capture the screen; crop and magnify to read
                          small type without downscaling anything
  shinogi wait-idle [s]   wait until the screen stops changing
  shinogi key <name>...   send keys (e.g. ret, esc, alt-x, shift-a)
  shinogi type <text>     send text a character at a time
  shinogi move <x> <y>    put the pointer at a pixel
  shinogi click [--right|--middle] [<x> <y>]
  shinogi dclick [<x> <y>]
  shinogi drag <x1> <y1> <x2> <y2>
  shinogi monitor <cmd>   raw QEMU monitor command
  shinogi qmp <json>      raw QMP command
  shinogi drive-c         print the path of the guest's drive C
  shinogi reset-drive-c   restore drive C to the shipped tree
  shinogi doctor          check the bundle against this host

Environment: SHINOGI_HOME (default ~/.shinogi), SHINOGI_DRIVE_C,
SHINOGI_CPU, SHINOGI_XRES, SHINOGI_YRES, SHINOGI_MEM,
SHINOGI_LOADER (auto|native|bundled), SHINOGI_DRAG_STEPS,
SHINOGI_CLICK_MS (gap between button transitions, default 40).

Coordinates are screen pixels, 0,0 at the top left of a ${XRES}x${YRES}
screen.
EOF
}

case "${1:-help}" in
    start) shift; cmd_start "$@" ;;
    run) shift; cmd_run "$@" ;;
    stop) shift; cmd_stop "$@" ;;
    status) shift; cmd_status "$@" ;;
    log) shift; cmd_log "$@" ;;
    errors) shift; cat "$ERRLOG" ;;
    screenshot|screendump) shift; cmd_screenshot "$@" ;;
    key) shift; cmd_key "$@" ;;
    type) shift; cmd_type "$@" ;;
    move) shift; cmd_move "$@" ;;
    click) shift; cmd_click "$@" ;;
    dclick|doubleclick) shift; cmd_dclick "$@" ;;
    drag) shift; cmd_drag "$@" ;;
    wait-idle|settle) shift; cmd_wait_idle "$@" ;;
    monitor) shift; mon "$@" ;;
    qmp) shift; qmp "$1" ;;
    drive-c) echo "$DRIVE_C" ;;
    reset-drive-c) shift; cmd_reset_drive_c "$@" ;;
    doctor) shift; cmd_doctor "$@" ;;
    help|-h|--help) cmd_help ;;
    *) echo "shinogi: unknown command '$1'" >&2; echo >&2; cmd_help >&2; exit 2 ;;
esac

#!/usr/bin/env bash
#
# make-mint-install.sh - shinogi's drive C: FreeMiNT's own bootable
# snapshot, installed the way FreeMiNT says to install it, plus the few
# files this machine needs that the snapshot cannot carry.
#
# FreeMiNT's install, from the snapshot page its GitHub README links to
# (https://atari.joska.no/snapshots/freemint/bootable/):
#
#   1. download the archive suitable for your machine
#   2. unpack it to the root of your C drive
#   3. disable all AUTO programs except the archive's mint-*.prg
#   4. reboot
#
# Steps 1 and 2 are done here as written, with the TT/Falcon archive: its
# kernel modules, XaAES and userland are built for 68020-68060, which is
# both editions. What it brings: XaAES, TeraDesk as the desktop, bash and
# the Unix tools, QED, COPS, TosWin2 and the hypertext guides.
#
# Then these changes, each explained where it is made:
#
#   AUTO/MINT.PRG          our kernel, in place of the archive's loader
#                          and stock kernels, none of which can reach a
#                          GEMDOS-level drive C
#   MINT/1-19-CUR/         the archive's MINT/1-19-<id>, renamed to the
#                          directory our kernel looks for
#   .../INET4.XDD          the IP stack, built with our kernel, and our
#   .../VIRTIONE.XIF       virtio network driver
#   .../MINT.CNF           /etc built in the ram disk; the NAT network
#   EMUTOS.IMG             the firmware the launcher boots
#   AUTO/FVDI.PRG          fVDI, with FVDI.SYS and GEMSYS/
#   TOOLS/SETRES.PRG       the resolution picker
#   XAAES theme            the a_skin1 widgets and ablue gradients
#
# The tree is uppercase and 8.3-clean throughout, and both are checked
# before exit. See docs/freemint-build.md.
#
# Usage:
#   tools/make-mint-install.sh [--zip FILE] [--freemint DIR] [--out DIR]
#                              [--build]
#
#   --zip FILE      a FreeMiNT bootable snapshot to use instead of the
#                   pinned one below (taken as given, no checksum)
#   --freemint DIR  FreeMiNT source tree the kernel, IP stack and network
#                   driver are built in (default $FREEMINT_DIR or
#                   $HOME/git/freemint-rc)
#   --out DIR       where to write the tree (default $HOME/shinogi-build/
#                   mint-install); recreated from scratch each run
#   --build         build the kernel, IP stack and network driver first

set -euo pipefail

FREEMINT="${FREEMINT_DIR:-$HOME/git/freemint-rc}"
OUT="$HOME/shinogi-build/mint-install"
ZIP=
DO_BUILD=no

while [ $# -gt 0 ]; do
    case "$1" in
        --zip)      ZIP="$2"; shift 2 ;;
        --freemint) FREEMINT="$2"; shift 2 ;;
        --out)      OUT="$2"; shift 2 ;;
        --build)    DO_BUILD=yes; shift ;;
        -h|--help)  sed -n '2,49p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

# The snapshot, pinned. FreeMiNT has had no release since 1.18.0 (2013)
# and its own site points at these snapshots instead. "latest" moves with
# every upstream commit, so the build names one: upstream 0218382, built
# 2026-10-19, identical to "latest" on the day it was pinned. The file
# name carries the commit; the sysdir inside carries its first three hex
# digits (1-19-021).
SNAPSHOT=freemint-1-19-0218382e-tt_falcon_clones.zip
SNAPSHOT_SHA256=0529ec5fa9a88298b7e0ad4640777729b48bd13240d8069bd207457c8350fbcb
SNAPSHOT_URL="https://atari.joska.no/snapshots/freemint/bootable/$SNAPSHOT"
CACHE="$HOME/shinogi-build/cache"

# Our kernel.
#
# It has to be ours. Every kernel in the snapshot takes GEMDOS away from
# TOS and drives FAT itself over XHDI, and shinogi's drive C is a host
# folder served at GEMDOS level, with no BIOS device behind it at all. A
# stock kernel loads, then finds no drive C -- its own boot drive. The
# kernel FreeMiNT builds for Hatari has the same problem and the same
# answer, -DOLDTOSFS, which keeps the kernel calling the TOS GEMDOS.
# -DMACHINE_QEMU_VIRT is what lets Shut Down power the machine off.
#
# One 020-060 kernel serves both editions; -DM68040 picks the runtime MMU
# and cache model, not the instruction set. docs/freemint-build.md has
# the history of this target.
KERNEL_TARGET=hat02060
KERNEL_CPU=02060
KERNEL_PRG=mint0206h.prg
KERNEL_DEFS='-DCRYPTO_CODE -DSOFT_UNITABLE -DBUILTIN_SHELL -DM68040 -DOLDTOSFS -DWITH_MMU_SUPPORT -DMACHINE_QEMU_VIRT'
MOD_TARGET=02060

# The directory our kernel reads: MINT_VERS_PATH_STRING in
# sys/buildinfo/version.h, "1-19-cur" for a development build. Upstream's
# CI swaps the "cur" for a commit prefix, which is why the snapshot's
# sysdir is called something else.
SYSDIR_NAME=1-19-cur

[ -d "$FREEMINT/sys" ] || { echo "not a FreeMiNT tree: $FREEMINT" >&2; exit 1; }

if [ "$DO_BUILD" = yes ]; then
    export M68K_ATARI_MINT_CROSS=yes
    # FreeMiNT builds leave a lot of intermediates behind; keep them off
    # any small /tmp.
    export TMPDIR="${TMPDIR:-$HOME/shinogi-build/tmp}"
    mkdir -p "$TMPDIR"

    make -C "$FREEMINT/sys" \
        "kerneltargets=$KERNEL_TARGET" \
        "CPU_$KERNEL_TARGET=$KERNEL_CPU" \
        "MINT_$KERNEL_TARGET=$KERNEL_PRG" \
        "KERNELDEFS_$KERNEL_TARGET=$KERNEL_DEFS" \
        "$KERNEL_TARGET"
    make -C "$FREEMINT/sys/sockets" "$MOD_TARGET"
    # The xif Makefiles use their own target names, and 'make 02060'
    # there succeeds while building nothing. The default target builds
    # every CPU variant, 02060 among them.
    make -C "$FREEMINT/sys/sockets/xif/virtio_net"
fi

need() {
    [ -f "$1" ] || { echo "missing: $1" >&2
                     echo "(run with --build, or build FreeMiNT first)" >&2
                     exit 1; }
}

KERNEL="$FREEMINT/sys/.compile_$KERNEL_TARGET/$KERNEL_PRG"
INET4="$FREEMINT/sys/sockets/.compile_$MOD_TARGET/inet4.xdd"
VNET="$FREEMINT/sys/sockets/xif/virtio_net/.compile_$MOD_TARGET/virtio_net.xif"
need "$KERNEL"
need "$INET4"
need "$VNET"

# ------------------------------------------------ 1. download the archive

if [ -z "$ZIP" ]; then
    ZIP="$CACHE/$SNAPSHOT"
    if [ ! -f "$ZIP" ]; then
        mkdir -p "$CACHE"
        curl -fsSL --retry 3 -o "$ZIP.part" "$SNAPSHOT_URL"
        mv "$ZIP.part" "$ZIP"
    fi
    echo "$SNAPSHOT_SHA256  $ZIP" | sha256sum -c --quiet - || {
        echo "$ZIP does not match the pinned checksum" >&2; exit 1; }
fi
[ -f "$ZIP" ] || { echo "no such snapshot: $ZIP" >&2; exit 1; }

# ----------------------------------------- 2. unpack it to the root of C:

if [ -e "$OUT" ]; then
    # No rm: walk the tree bottom-up and unlink explicitly.
    python3 - "$OUT" <<'PY'
import os, sys
root = sys.argv[1]
for dirpath, dirnames, filenames in os.walk(root, topdown=False):
    for name in filenames:
        os.remove(os.path.join(dirpath, name))
    for name in dirnames:
        os.rmdir(os.path.join(dirpath, name))
os.rmdir(root)
PY
fi
mkdir -p "$OUT"
unzip -q "$ZIP" -d "$OUT"
echo "snapshot: $(basename "$ZIP")"

# The archive's sysdir, renamed to the one our kernel reads. Everything
# inside it that names the directory does so through ${SYSDIR}, so the
# rename needs no edits to follow it.
shopt -s nullglob
sysdirs=("$OUT"/mint/1-19-*)
shopt -u nullglob
[ "${#sysdirs[@]}" -eq 1 ] || {
    echo "expected one mint/1-19-* directory in the snapshot, found ${#sysdirs[@]}" >&2
    exit 1; }
MINTDIR="$OUT/mint/$SYSDIR_NAME"
if [ "${sysdirs[0]}" != "$MINTDIR" ]; then
    mv "${sysdirs[0]}" "$MINTDIR"
fi
echo "sysdir: mint/$(basename "${sysdirs[0]}") -> mint/$SYSDIR_NAME"
XAAESDIR="$MINTDIR/xaaes"
SYSROOT="$MINTDIR/sys-root"
need "$MINTDIR/mint.cnf"
need "$XAAESDIR/xaaes.cnf"

# ----------------------------------------------------------- the kernel
#
# Step 3 says to leave only the archive's mint-*.prg in AUTO. That
# program is a loader: it reads the _CPU and _MCH cookies and starts one
# of the stock kernels from the sysdir. None of those can reach drive C
# (see the top), so the loader and the kernels it chooses between are
# taken out, and our kernel is the AUTO program instead.
for f in "$OUT"/auto/mint-*.prg "$MINTDIR"/mint*.prg; do
    [ -f "$f" ] && python3 -c "import os,sys; os.remove(sys.argv[1])" "$f"
done
cp "$KERNEL" "$OUT/auto/mint.prg"
echo "kernel: auto/mint.prg ($KERNEL_TARGET, $(stat -c %s "$KERNEL") bytes)"

# ---------------------------------------------------------- the network
#
# The IP stack and the driver are replaced as a pair, both from the tree
# the kernel came from. A network driver is compiled against the stack's
# interface structures, and the two trees do not have the same ones: ours
# carries the interface-index work, upstream's does not. A driver built
# against one stack and loaded by the other is the kind of mismatch that
# crashes rather than complains.
cp "$INET4" "$MINTDIR/inet4.xdd"
cp "$VNET" "$MINTDIR/virtione.xif"
echo "network: inet4.xdd + virtione.xif from $(basename "$FREEMINT")"

# Switched off, not removed -- the snapshot's own convention, as with its
# asix.xix and isofs.xfx: renaming the extension back turns one on.
#
# These drive hardware this machine does not have: DMA sound, the floppy
# controller, and the parallel and serial ports that plip and slip run
# over. Upstream's ARAnyM archive -- its own build for a machine that is
# an emulator -- carries none of the four. plip and slip have a second
# reason: inet4 loads every *.xif at boot, and these two were built
# against the upstream stack, not ours.
disable() {                     # <name> <new extension>
    [ -f "$MINTDIR/$1" ] || return 0
    mv "$MINTDIR/$1" "$MINTDIR/${1%.*}.$2"
    echo "  disabled: $1 -> ${1%.*}.$2"
}
disable audiodev.xdd xdx
disable flop_raw.xdd xdx
disable plip.xif xix
disable slip.xif xix

# ------------------------------------------------------------ 8.3 names
#
# Drive C is a GEMDOS world, and a name that cannot be spelled in 8.3
# cannot be opened either: it shows up in a listing, truncated, and every
# access fails. The snapshot is built for FAT drives with long-name
# support and has a dozen such names, all under sys-root.
#
# Commands are renamed so they stay usable. resolv.conf is replaced
# outright below. Everything else is dropped, by name, so the log says
# what this tree does not have.
for pair in "bin/fsck.minix:bin/fsckminx" \
            "bin/mount_nfs:bin/mountnfs" \
            "bin/mount_nfs3:bin/mntnfs3"; do
    from="$SYSROOT/${pair%%:*}"
    to="$SYSROOT/${pair##*:}"
    if [ -f "$from" ]; then
        mv "$from" "$to"
        echo "  renamed: sys-root/${pair%%:*} -> ${pair##*:}"
    fi
done

# ---------------------------------------------------------------- /etc
#
# The resolver opens "/etc/resolv.conf" by that exact name, and drive C
# cannot spell it: the four-character extension comes out truncated and
# the file cannot be opened at all. The resolver then falls back to its
# compiled-in name server, 127.0.0.1, and every lookup fails having put
# nothing on the wire -- which looks like a broken network and is not
# one. host.conf has the same problem.
#
# So /etc is built at boot in u:/ram, the kernel's own ram filesystem,
# which takes long names, from 8.3-named copies kept in sys-root/etc.
# Redirection does the copying rather than cp, which refuses files on
# this drive, reporting them "replaced while being copied".
ETC="$SYSROOT/etc"
[ -f "$ETC/resolv.conf" ] && python3 -c "import os,sys; os.remove(sys.argv[1])" "$ETC/resolv.conf"
# 10.0.2.3 is the name server on the NAT link every launcher gives the
# guest; it forwards to whatever the host uses. 8.8.8.8 is the
# snapshot's own choice, kept as the fallback.
printf 'nameserver 10.0.2.3\nnameserver 8.8.8.8\n' > "$ETC/resolv.cnf"
printf 'order hosts,bind\n'                        > "$ETC/hostconf"
printf '127.0.0.1 localhost\n'                     > "$ETC/hosts"

cat > "$MINTDIR/mketc.sh" <<'MKETC'
# Build /etc where long names are possible. Run from mint.cnf.
# LEFT is the name drive C can spell, RIGHT the name programs ask for.
E=/ram/etc
S=/c/mint/1-19-cur/sys-root/etc
mkdir -p $E
cat $S/resolv.cnf > $E/resolv.conf
cat $S/hostconf   > $E/host.conf
cat $S/hosts      > $E/hosts
cat $S/passwd     > $E/passwd
cat $S/fstab      > $E/fstab
cat $S/profile    > $E/profile
MKETC

python3 - "$MINTDIR/mint.cnf" <<'PY'
import sys

path = sys.argv[1]
s = open(path, encoding='latin-1').read()

def swap(old, new):
    global s
    if s.count(old) != 1:
        sys.exit("mint.cnf: expected exactly one %r" % old.strip())
    s = s.replace(old, new)

swap('sln ${SYSDIR}sys-root/etc      u:/etc\n',
     '# shinogi: not a link to sys-root/etc on drive C, which cannot spell\n'
     '# names like resolv.conf. /etc is built in the ram filesystem by\n'
     '# mketc.sh, run under EXECUTE PROGRAMS below.\n'
     'sln u:/ram/etc                 u:/etc\n')

anchor = '#exec u:/bin/route add default XXX gw BBB.BBB.BBB.BBB\n'
swap(anchor, anchor +
     '\n'
     '# shinogi: /etc first (see the sln above), then the network. The\n'
     '# launchers give the guest a NAT link whose addressing is fixed:\n'
     '# 10.0.2.15 for us, 10.0.2.2 the gateway, 10.0.2.3 the name server.\n'
     '# On a bridged or tap link these are wrong, and so is the name server\n'
     '# in sys-root/etc/resolv.cnf.\n'
     'exec u:/bin/bash u:/c/mint/1-19-cur/mketc.sh\n'
     'exec u:/bin/ifconfig eth0 addr 10.0.2.15 netmask 255.255.255.0 up\n'
     'exec u:/bin/route add default eth0 gw 10.0.2.2\n')

open(path, 'w', encoding='latin-1').write(s)
PY
echo "mint.cnf: /etc in u:/ram, eth0 10.0.2.15 via 10.0.2.2"

# -------------------------------------------------------------- EmuTOS
#
# The firmware. QEMU loads it with -kernel, off the host filesystem,
# before any guest exists, and the launcher looks for it here first --
# so a user upgrading EmuTOS drops a new file in place. The guest never
# reads it. A deleted EMUTOS.IMG means nothing boots at all.
EMUTOS_ELF="${EMUTOS_ELF:-$HOME/git/emutos/emutos-virt.elf}"
if [ -f "$EMUTOS_ELF" ]; then
    cp "$EMUTOS_ELF" "$OUT/emutos.img"
    echo "firmware: emutos.img ($(stat -c %s "$EMUTOS_ELF") bytes)"
else
    echo "note: no EmuTOS at $EMUTOS_ELF - the packagers supply it" >&2
fi

# ---------------------------------------------------------------- fVDI
#
# fVDI replaces the ROM VDI and has to be resident before anything draws
# through it, so it goes in AUTO ahead of the kernel: EmuTOS runs AUTO in
# directory order, and FVDI.PRG sorts before MINT.PRG.
#
# fvdi.sys, read from the boot drive root:
#   booted      required when started from AUTO
#   cookie      the VALUE matters: a bare "cookie nvdi" yields 0x0250,
#               below the bar applications test -- HighWire wants NVDI
#               >= 3. "$" is hex to fVDI's atol().
#   01r ...     the 16-bit driver, looked for in gemsys\
# No "debug" line: to make fVDI talk, put "debugout 1" as the FIRST line
# and it reaches the host serial log.
FVDI="${FVDI_DIR:-$HOME/git/fvdi/fvdi}"
need "$FVDI/engine/fvdi_gnu.prg"
need "$FVDI/drivers/16_bit/16_bit.sys"
cp "$FVDI/engine/fvdi_gnu.prg" "$OUT/auto/fvdi.prg"
mkdir -p "$OUT/gemsys"
cp "$FVDI/drivers/16_bit/16_bit.sys" "$OUT/gemsys/16_bit.sys"
cat > "$OUT/fvdi.sys" <<'FVDICFG'
booted
cookie nvdi = $0501
01r 16_bit.sys
FVDICFG

# Outline fonts. "fonts" needs an "=" -- without one fVDI abandons the
# whole config and the driver line above never runs. load_font_dir()
# recurses, so naming the top directory picks up every family below it.
FONTS_SRC="${FONTS_DIR:-$HOME/git/Aranym/lan-share/C-drive/gemsys/fonts}"
if [ -d "$FONTS_SRC" ]; then
    mkdir -p "$OUT/gemsys/fonts"
    cp -r "$FONTS_SRC"/. "$OUT/gemsys/fonts/"
    printf 'fonts = c:\\gemsys\\fonts\n' >> "$OUT/fvdi.sys"
    echo "vdi: fVDI, 16-bit driver, fonts ($(du -sh "$OUT/gemsys/fonts" | cut -f1))"
else
    echo "vdi: fVDI, 16-bit driver; no fonts at $FONTS_SRC, system font only" >&2
fi

# --------------------------------------------------- resolution picker
#
# The screen size is fixed when QEMU starts, so SETRES writes the choice
# to C:\SHINOGI.INI and the launcher reads it on the next start. Built
# here so it cannot drift from the launcher about the file format.
GUEST_SRC="$(cd "$(dirname "$0")" && pwd)/guest/setres.c"
mkdir -p "$OUT/tools"
m68k-atari-mint-gcc -O2 -o "$OUT/tools/setres.prg" "$GUEST_SRC"
echo "resolution picker: tools/setres.prg"

# ---------------------------------------------------------- XaAES theme
#
# The snapshot's XaAES starts TeraDesk as its shell and TosWin2 beside
# it; both stay as they are. The look is ours: the a_skin1 widget set and
# the ablue/agrey gradients, which are not in stock XaAES and come off
# the ARAnyM drive. Appended, because the last assignment wins.
#
# textures is OFF on purpose: the metallic look comes from the widget
# set, and "textures = img" gives a different desktop entirely.
THEME="${THEME_DIR:-$HOME/git/Aranym/lan-share/host_fs/Bootback/c/mint/1-19-025/xaaes}"
if [ -f "$THEME/widgets/a_skin1.rsc" ]; then
    cp -f "$THEME/widgets"/a_skin*.rsc "$XAAESDIR/widgets/"
    cp -f "$THEME/gradient"/ablue_*.grd "$THEME/gradient"/agrey_*.grd \
          "$XAAESDIR/gradient/"
    cat >> "$XAAESDIR/xaaes.cnf" <<'XCNF'

# --- Shinogi appearance ------------------------------------------------
widgets = widgets\a_skin1.rsc
gradients = ablue_2
textures = 0
palette = nvdi
back_col = 1
XCNF
    echo "theme: a_skin1 widgets, ablue_2 gradients"
else
    echo "note: no theme at $THEME - the snapshot's stock look" >&2
fi

# ------------------------------------------- drop what 8.3 cannot spell
#
# Last, so it also catches anything added above. Each drop is named, so
# the log says what this tree does not have; a directory the drop
# empties goes too, while one that was empty in the archive
# (sys-root/tmp, sys-root/root) stays.
python3 - "$OUT" <<'PY'
import os, re, shutil, sys

root = sys.argv[1]
ok = re.compile(r"^[A-Za-z0-9_~%^&@!(){}'`#$-]{1,8}(\.[A-Za-z0-9_~%^&@!(){}'`#$-]{1,3})?$")
for dirpath, dirnames, filenames in os.walk(root, topdown=False):
    dropped_here = False
    for name in filenames + dirnames:
        if ok.match(name):
            continue
        p = os.path.join(dirpath, name)
        if os.path.isdir(p) and not os.path.islink(p):
            shutil.rmtree(p)
        else:
            os.remove(p)
        dropped_here = True
        print("  dropped (not 8.3):", os.path.relpath(p, root))
    if dropped_here and dirpath != root and not os.listdir(dirpath):
        os.rmdir(dirpath)
        print("  dropped (left empty):", os.path.relpath(dirpath, root))
PY

# ----------------------------------------------- uppercase + 8.3 check
#
# The shinogi drive C folder is uppercase throughout, and the packagers
# write into it by uppercase paths (AUTO/MINT.PRG, MINT/1-19-CUR/...).
# The kernel and XaAES ask for their files in lower case; hostfsd folds
# case the way GEMDOS does, so both find them.
python3 - "$OUT" <<'PY'
import os, sys

root = sys.argv[1]
for dirpath, dirnames, filenames in os.walk(root, topdown=False):
    for name in filenames + dirnames:
        upper = name.upper()
        if upper != name:
            os.rename(os.path.join(dirpath, name),
                      os.path.join(dirpath, upper))
PY

python3 - "$OUT" <<'PY'
import os, re, sys

root = sys.argv[1]
ok = re.compile(r'^[A-Za-z0-9_!#$%&()@^{}~\'`\-]{1,8}(\.[A-Za-z0-9_!#$%&()@^{}~\'`\-]{1,3})?$')
bad = []
for dirpath, dirnames, filenames in os.walk(root):
    for name in dirnames + filenames:
        if not ok.match(name):
            bad.append(os.path.relpath(os.path.join(dirpath, name), root))
if bad:
    print("NOT 8.3-clean:", file=sys.stderr)
    for name in sorted(bad):
        print("  " + name, file=sys.stderr)
    sys.exit(1)
print("8.3 check: all names clean")
PY

echo
echo "drive C tree: $OUT ($(du -sh "$OUT" | cut -f1))"
echo "The packagers take it zipped, with freemint-install/ at the top."

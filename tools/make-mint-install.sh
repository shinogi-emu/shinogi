#!/usr/bin/env bash
#
# make-mint-install.sh - assemble a FreeMiNT system install tree for shinogi.
#
# Produces the directory layout that EmuTOS/FreeMiNT expect on the boot
# drive (drive C), ready to be copied into the shinogi drive C folder:
#
#   AUTO/MINT.PRG               the 68040 kernel, launched from AUTO by EmuTOS
#   MINT/1-19-CUR/              the kernel "sysdir": config + loadable modules
#   MINT/1-19-CUR/XAAES/        XaAES (the AES/GEM layer) and its resources
#
# Every name written here is uppercase and 8.3-clean; both are verified
# before exit.
# See docs/freemint-build.md for the reasoning behind the choices.
#
# Usage:
#   tools/make-mint-install.sh [--freemint DIR] [--out DIR] [--build]
#
#   --freemint DIR  FreeMiNT source tree (default $FREEMINT_DIR or
#                   $HOME/git/freemint)
#   --out DIR       where to write the tree (default $HOME/shinogi-build/
#                   mint-install); recreated from scratch each run
#   --build         run the FreeMiNT make targets first; without it the
#                   tree is assembled from whatever is already compiled

set -euo pipefail

FREEMINT="${FREEMINT_DIR:-$HOME/git/freemint}"
OUT="$HOME/shinogi-build/mint-install"
DO_BUILD=no

while [ $# -gt 0 ]; do
    case "$1" in
        --freemint) FREEMINT="$2"; shift 2 ;;
        --out)      OUT="$2"; shift 2 ;;
        --build)    DO_BUILD=yes; shift ;;
        -h|--help)  sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

# The kernel we ship.
#
# Upstream has no 68040 target that suits us, so we define one on the
# make command line (FreeMiNT's sys/KERNELDEFS documents custom targets;
# the variables below are exactly what a new entry in that file would
# set, so nothing in the FreeMiNT tree has to be modified).
#
#   -DM68040 -DWITH_MMU_SUPPORT   68040 with MMU, matching QEMU virt
#   -DOLDTOSFS                    keep calling the underlying TOS GEMDOS
#                                 instead of driving a real FAT filesystem
#                                 over XHDI. This is the flag upstream uses
#                                 for its Hatari kernels, and it is what
#                                 makes a GEMDOS-level emulated drive C
#                                 (which is exactly what shinogi's hostfs
#                                 is) reachable at all.
#
# We deliberately leave out -DWITH_NATIVE_FEATURES: upstream's hat targets
# enable it for ARAnyM/Hatari NatFeats, which QEMU virt does not provide.
KERNEL_TARGET=hat040
KERNEL_CPU=040
KERNEL_PRG=mint040h.prg
KERNEL_DEFS='-DCRYPTO_CODE -DSOFT_UNITABLE -DBUILTIN_SHELL -DM68040 -DOLDTOSFS -DWITH_MMU_SUPPORT'

# The stock 68040 kernel is built and shipped alongside it, unused, so
# the FAT/XHDI path can be tried once shinogi grows a block device.
ALT_TARGET=040
ALT_PRG=mint040.prg

# Loadable modules and XaAES are built per CPU *family*, not per CPU; the
# family covering the 68040 is 02060 (68020 through 68060).
MOD_TARGET=02060

# Must match MINT_VERS_PATH_STRING in sys/buildinfo/version.h.
SYSDIR_NAME=1-19-cur

[ -d "$FREEMINT/sys" ] || { echo "not a FreeMiNT tree: $FREEMINT" >&2; exit 1; }

XA="$FREEMINT/xaaes/src.km"

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
    make -C "$FREEMINT/sys" "$ALT_TARGET"
    make -C "$XA" "$MOD_TARGET"
    make -C "$XA/xaloader" "$MOD_TARGET"
    make -C "$XA/adi/whlmoose" "$MOD_TARGET"
    make -C "$XA/gradient"
    for d in sys/xdd/xconout2 sys/xdd/nfstderr sys/xfs/ext2fs \
             sys/xfs/minixfs sys/xfs/nfs sys/sockets; do
        make -C "$FREEMINT/$d" "$MOD_TARGET"
    done
fi

# ---------------------------------------------------------------- layout

AUTODIR="$OUT/auto"
MINTDIR="$OUT/mint/$SYSDIR_NAME"
XAAESDIR="$MINTDIR/xaaes"
FONTSDIR="$MINTDIR/fonts"
TBLDIR="$MINTDIR/keyboard"

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
mkdir -p "$AUTODIR" "$MINTDIR" "$XAAESDIR" "$FONTSDIR" "$TBLDIR"

need() {
    [ -f "$1" ] || { echo "missing build output: $1" >&2
                     echo "(run with --build, or build FreeMiNT first)" >&2
                     exit 1; }
}

# --- the kernel ---------------------------------------------------------
#
# EmuTOS runs every *.PRG in \AUTO at boot, in directory order. The
# FreeMiNT kernel is itself the AUTO program - we do not use
# tools/mintload, whose job is to sniff the machine and pick one of
# several kernels, because we ship exactly one and its machine detection
# has nothing to go on here.
KERNEL="$FREEMINT/sys/.compile_$KERNEL_TARGET/$KERNEL_PRG"
need "$KERNEL"
cp "$KERNEL" "$AUTODIR/mint.prg"
# A second copy in the sysdir, so the tree still records which kernel it
# was built from once the AUTO copy is renamed or replaced.
cp "$KERNEL" "$MINTDIR/$KERNEL_PRG"
# The FAT/XHDI kernel, parked in the sysdir. Only one kernel may be
# active, so this one stays out of AUTO.
ALT="$FREEMINT/sys/.compile_$ALT_TARGET/$ALT_PRG"
need "$ALT"
cp "$ALT" "$MINTDIR/$ALT_PRG"

# --- kernel configuration ----------------------------------------------
#
# The kernel looks for \mint\1-19-cur\ on the boot drive and reads
# mint.cnf from it. The stock example starts XaAES; we start MyAES
# instead, from where its readme puts it.
need "$FREEMINT/doc/examples/mint.cnf"
sed -e 's|^#setenv LOGNAME root|setenv LOGNAME root|' \
    -e 's|^#setenv USER    root|setenv USER    root|' \
    -e 's|^#setenv HOME    /root|setenv HOME    /root|' \
    -e "s|^GEM=.*|GEM=c:\\\\gemsys\\\\myaes\\\\myaes020.prg|" \
    "$FREEMINT/doc/examples/mint.cnf" > "$MINTDIR/mint.cnf"

# --- loadable modules ---------------------------------------------------
#
# Anything named *.xfs / *.xdd in the sysdir is loaded at boot. Modules we
# do not want active are shipped with the conventional disabled extensions
# (.xfx / .xdx) so they can be turned on by renaming.
cp_mod() { need "$1"; cp "$1" "$2"; }
cp_mod "$FREEMINT/sys/xdd/xconout2/.compile_$MOD_TARGET/xconout2.xdd" "$MINTDIR/xconout2.xdd"
cp_mod "$FREEMINT/sys/xfs/ext2fs/.compile_$MOD_TARGET/ext2.xfs"       "$MINTDIR/ext2.xfs"
cp_mod "$FREEMINT/sys/xfs/minixfs/.compile_$MOD_TARGET/minix.xfs"     "$MINTDIR/minix.xfx"
# No FreeMiNT network driver exists for virtio-net, so the IP stack and
# the NFS client have nothing to bind to: shipped disabled.
cp_mod "$FREEMINT/sys/sockets/.compile_$MOD_TARGET/inet4.xdd"         "$MINTDIR/inet4.xdx"
cp_mod "$FREEMINT/sys/xfs/nfs/.compile_$MOD_TARGET/nfs.xfs"           "$MINTDIR/nfs.xfx"
# nfstderr routes kernel debug output through ARAnyM native features,
# which QEMU virt does not provide: shipped disabled.
cp_mod "$FREEMINT/sys/xdd/nfstderr/.compile_$MOD_TARGET/nfstderr.xdd" "$MINTDIR/nfstderr.xdx"

# --- XaAES --------------------------------------------------------------
cp_mod "$XA/xaloader/.compile_$MOD_TARGET/xaloader.prg" "$XAAESDIR/xaloader.prg"
cp_mod "$XA/.compile_$MOD_TARGET/xaaes020.km"           "$XAAESDIR/xaaes.km"
cp_mod "$XA/adi/whlmoose/.compile_$MOD_TARGET/moose.adi"   "$XAAESDIR/moose.adi"
cp_mod "$XA/adi/whlmoose/.compile_$MOD_TARGET/moose_w.adi" "$XAAESDIR/moose_w.adi"

cp "$XA"/*.rsc "$XAAESDIR/"
cp "$XA"/*.rsl "$XAAESDIR/"
cp "$XA"/xa_help.* "$XAAESDIR/"

mkdir -p "$XAAESDIR/gradient" "$XAAESDIR/widgets" "$XAAESDIR/xobj"
cp "$XA/gradient"/*.grd "$XAAESDIR/gradient/"
cp "$XA/widgets"/*.rsc  "$XAAESDIR/widgets/"
cp "$XA/xobj"/*.rsc     "$XAAESDIR/xobj/"
cp -r "$XA/img" "$XAAESDIR/"
cp -r "$XA/pal" "$XAAESDIR/"

# XaAES's own config. The stock example points "shell =" at TeraDesk,
# which we do not ship; leaving it commented out lets XaAES fall back to
# its built-in desktop.
need "$XA/example.cnf"
#
# launchpath: the stock example points at u:\opt\GEM, which belongs to a
# full distribution we do not ship -- with it, XaAES's built-in desktop
# offers an empty launcher and there is no way to start anything. Point
# it at the boot drive instead, which is the host folder the user
# actually puts programs in. doc/install.txt asks for exactly this kind
# of path adaptation when the layout differs from the stock snapshot.
sed -e 's|^#setenv AVSERVER   "DESKTOP "|setenv AVSERVER   "DESKTOP "|' \
    -e 's|^#setenv FONTSELECT "DESKTOP "|setenv FONTSELECT "DESKTOP "|' \
    -e 's|^\(launchpath[[:space:]]*=\).*|\1  c:\\|' \
    -e 's|^#shell = c:.teradesk.desktop.prg|shell = c:\\teradesk\\desktop.prg|' \
    "$XA/example.cnf" > "$XAAESDIR/xaaes.cnf"

# --- fonts and keyboard tables -----------------------------------------
cp -r "$FREEMINT/fonts"/* "$FONTSDIR/"
# Upstream ships a handful of names that are not 8.3; rename them the way
# FreeMiNT's own snapshot script does.
mv "$FONTSDIR/cs/cp1250_08.txt" "$FONTSDIR/cs/cp125008.txt"
mv "$FONTSDIR/cs/cp1250_09.txt" "$FONTSDIR/cs/cp125009.txt"
mv "$FONTSDIR/cs/cp1250_10.txt" "$FONTSDIR/cs/cp125010.txt"
mv "$FONTSDIR/pl/ISO-8859-2.fnt" "$FONTSDIR/pl/iso88592.fnt"

cp -r "$FREEMINT/sys/tbl"/* "$TBLDIR/"

# Drop names that cannot exist on an 8.3 GEMDOS drive.
#
# Used for the third-party trees, which carry documentation and icon sets
# with modern names -- gpl-3.0.txt, MYSTART48.PNG. A name that cannot be
# spelled in 8.3 cannot be asked for either, so the file is unreachable
# whether or not it is copied. Dropping is reported rather than silent,
# because a silent drop reads as though the file shipped.
prune_non_83()
{
    python3 - "$1" <<'PYX'
import os, re, shutil, sys
ok = re.compile(r"^[A-Za-z0-9_~%^&@!(){}'`#$-]{1,8}(\.[A-Za-z0-9_~%^&@!(){}'`#$-]{1,3})?$")
n = 0
for dirpath, dirnames, filenames in os.walk(sys.argv[1], topdown=False):
    for name in filenames:
        if not ok.match(name):
            os.remove(os.path.join(dirpath, name)); n += 1
    for name in dirnames:
        if not ok.match(name):
            shutil.rmtree(os.path.join(dirpath, name), ignore_errors=True); n += 1
print(n)
PYX
}

# --- desktop: Thing ----------------------------------------------------
#
# MyAES is an AES, not a desktop, and refuses to start without one -- its
# own desktop.cnf points at c:\thing\thing.app and says so.
#
# This is Thing Neo, the current line: Arno Welzel, then Thomas Binder,
# then Gerhard Stoll, and Olivier Landemarre since 2023 -- the same
# author as MyAES, which is why the two fit together. GPL, so shipping it
# is fine. Prebuilt, because the source needs PureC and our toolchain is
# gcc.
#
# The archive carries a 68000 build alongside the default; we take the
# default, matching the 68020 MyAES and the 68040 kernel.
THING="${THING_DIR:-$HOME/tmp/thingneo/thing}"
if [ -f "$THING/thing.app" ]; then
    mkdir -p "$OUT/thing"
    # Everything except the 68000 variant and the licence texts, whose
    # names (gpl-3.0.txt) cannot exist on an 8.3 drive anyway.
    for f in "$THING"/*; do
        b=$(basename "$f")
        case "$b" in
            68000|LICENSE) continue ;;
        esac
        cp -r "$f" "$OUT/thing/"
    done
    dropped=$(prune_non_83 "$OUT/thing")
    echo "desktop: thing/thing.app (Thing Neo), $dropped file(s) dropped as un-8.3"
else
    echo "note: no Thing at $THING - MyAES will have no desktop to start" >&2
fi

# --- AES: MyAES --------------------------------------------------------
#
# Replaces XaAES. Installed where its readme says -- C:\GEMSYS\MYAES --
# and mint.cnf is pointed at its kernel instead of xaloader.prg.
MYAES="${MYAES_DIR:-$HOME/git/Aranym/lan-share/C-drive/myaes099final}"
MYAES_CPU="${MYAES_CPU:-68020}"
if [ -d "$MYAES/config/$MYAES_CPU/myaes" ]; then
    mkdir -p "$OUT/gemsys"
    cp -r "$MYAES/config/$MYAES_CPU/myaes" "$OUT/gemsys/"
    # Its shell: Thing, which is what the stock config already selects.
    if [ -f "$OUT/gemsys/myaes/desktop.cnf" ]; then
        sed -i 's|^shell .*|shell c:\\thing\\thing.app|' \
            "$OUT/gemsys/myaes/desktop.cnf"
    fi
    # Olivier's extras: a taskbar, a resolution switcher, network setup.
    # A glob that matches nothing makes cp fail, and set -e would end the
    # build over an optional extra, so each is copied only if it is there.
    for g in yopla yoprez yopnet; do
        if [ -d "$MYAES/config/goodies/$g" ]; then
            mkdir -p "$OUT/gemsys/myaes/$g"
            find "$MYAES/config/goodies/$g" -maxdepth 1 \
                 \( -name '*.prg' -o -name '*.cnf' -o -name '*.rsc' \) \
                 -exec cp {} "$OUT/gemsys/myaes/$g/" \;
        fi
    done
    # MyAES ships colour-icon sets and keyboard tables whose names cannot
    # exist on a GEMDOS drive -- MYSTART48.PNG, TRASHEMPTY.PNG and the
    # like. Drop them rather than fail the build: they are decoration, and
    # a name that cannot be spelled in 8.3 cannot be asked for either.
    # Counted and reported, because a silent drop reads as "shipped".
    dropped=$(prune_non_83 "$OUT/gemsys/myaes")
    echo "aes: gemsys/myaes ($MYAES_CPU), $dropped file(s) dropped as un-8.3"
else
    echo "note: no MyAES at $MYAES/config/$MYAES_CPU - keeping XaAES" >&2
fi

# ------------------------------------------------- uppercase + 8.3 check
#
# Drive C is an 8.3 GEMDOS world, and the shinogi drive C folder is
# uppercase throughout, so the tree is normalised to uppercase last.
# (The kernel and XaAES ask for their own files in lower case, so the
# GEMDOS layer has to fold case either way -- real GEMDOS does.)
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

# The guest reaches drive C through a GEMDOS layer that is 8.3 only.
# Anything longer, or with more than one dot, or with a character GEMDOS
# rejects, will simply not be found at boot.
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

echo "FreeMiNT install tree: $OUT"
echo "kernel: AUTO/MINT.PRG ($(stat -c %s "$OUT/AUTO/MINT.PRG") bytes, target $KERNEL_TARGET)"
# --- desktop -----------------------------------------------------------
#
# XaAES is the AES; it is not a desktop. Upstream's ready-to-go snapshot
# bundles TeraDesk for this, but the FreeMiNT source tree does not carry
# it, so a build from source comes up with XaAES running and nothing to
# launch. Ship TeraDesk and point XaAES's "shell =" at it -- the line its
# own example config already carries, commented out.
# Also drop it on the LAN share, which is how it reaches the Windows box.
# Without this the tree only ever exists on the build machine.
SHARE="${SHINOGI_SHARE:-$HOME/git/Aranym/lan-share}"
if [ -d "$SHARE" ]; then
    python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1], ignore_errors=True)" \
        "$SHARE/freemint-install"
    cp -r "$OUT" "$SHARE/freemint-install"
    ( cd "$SHARE" &&
      python3 -c "import os; os.path.exists('freemint-install.zip') and os.remove('freemint-install.zip')" &&
      zip -qr freemint-install.zip freemint-install )
    echo "also copied to $SHARE/freemint-install (+ .zip)"
fi

echo
echo "Copy into the shinogi drive C folder, e.g.:"
echo "  cp -r \"$OUT\"/* \"\${SHINOGI_HOSTFS:-\$HOME/shinogi-drive-c}\"/"

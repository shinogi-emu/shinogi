#!/usr/bin/env python3
#
# Rebuild the complete host-folder fixture set the goldens assert against.
#
# Why this exists: the fixtures were created ad hoc, task by task, and a
# later session reset one of the folders to stage a demo -- silently
# deleting FIXED.TXT and breaking tests/golden/phase5-dates, which had
# passed for hours. Nothing recorded what the folders were supposed to
# contain, so the breakage only surfaced on a full golden run.
#
# Everything the goldens depend on is now declared here. Run this and
# the fixtures are correct by construction.
#
#   tools/make-fixtures.py                 both fixture folders
#   tools/make-fixtures.py /some/dir       just that one
#
# Default folders:
#   /tmp/shinogi-hostfs    what tools/run-golden.sh mounts over 9P
#   ~/shinogi-drive-c      what tools/run-shinogi.sh mounts interactively
#   /tmp/shinogi-vvfat     what run-golden.sh hands to QEMU's vvfat
#                          driver as a virtio-blk device
#
# The two are kept identical so an interactive run shows exactly what the
# goldens verified.
#
# Each golden is compared against the guest's serial output filtered
# through one regex. The regexes are NOT derivable from the golden files,
# so they are recorded here and in tools/run-all-goldens.sh, which runs
# the whole set:
#
# phase5-readdir is compared as a SET, not as a sequence: its lines come
# straight out of the host's readdir(), whose order POSIX does not
# define and which really does differ -- this folder on tmpfs lists in
# creation order and on ext4 with dir_index in hash order. Every other
# listing golden is over output the GUEST sorted and is compared in
# order, because that order is the guest's own.
#
#   phase5-attach   9p: (slot [0-9]+ msize|attached,).*
#   phase5-walk     9p: (walk|getattr).*
#   phase5-readdir  9p: dirent.*                        (sorted)
#   phase5-drive    hostfs: drive C registered
#   phase5-dates    hostfs: date FIXED.TXT.*
#   phase5-read     hostfs: (open|read|seek|close|wrap) .*
#   phase5-bigread  hostfs: big .*
#   phase5-paths    hostfs: (rwopen|rwread|rwwrite|rwclose|setpath|getpath|
#                             dfree|escape|fattrib|setdrv|specname|longname|dtaguard).*
#   phase5-subdir   hostfs: sub.*
#   phase5-listing  hostfs: fs(first|next).*
#   phase5-dta      hostfs: dta.*
#   phase5-many     hostfs: many.*
#   phase5-vvfat    vblk: .*
#
# phase5-write is deliberately absent from that list. It belongs to
# tools/check-hostfs-write.py, which runs the guest against a THROWAWAY
# folder built by build_hostfs_write() below -- the WTEST directory in
# it is what arms the guest's write self-test, and it must never appear
# in the folders above.

import calendar
import os
import shutil
import subprocess
import sys

# FIXED.TXT carries a deliberately pre-1980 timestamp: the packed GEMDOS
# date field cannot express a year below 1980, and the reference
# implementation clamps the YEAR to 0 while leaving month and day intact
# rather than snapping to 1980-01-01. phase5-dates pins that behaviour,
# so this timestamp must not drift.
#
# The instant is chosen in UTC, not in local time. The guest has no
# timezone database and renders an mtime as UTC (see the comment in
# bdos/hostfs.c), so a fixture stamped with time.mktime() would make
# phase5-dates assert a different packed time on every machine that ran
# it -- which is what happened: a golden written on a UTC-4 host read
# 10:20:30 there and 14:20:30 everywhere else.
FIXED_MTIME = calendar.timegm((1975, 6, 15, 10, 20, 30, 0, 0, 0))

# A name too long for the guest's HOSTFS_MAX_NAME (64 including the
# terminator). It must be listed as absent AND must not hide the entries
# that follow it -- a 9P directory walker that treats "cannot use this
# entry" as "end of page" makes everything after it invisible. In raw
# byte order this sorts between "a.b.c" and "verylongname.txt", so the
# listing goldens show an entry on either side of it.
TOOLONG_NAME = "b" * 66 + ".txt"        # 70 characters

ROOT_FILES = {
    "HELLO.TXT":         "hello\n",
    "README.TXT":        "shinogi host folder\n",
    "verylongname.txt":  "long name test\n",
    "a.b.c":             "dots\n",
    "FIXED.TXT":         "fixed timestamp\n",
    TOOLONG_NAME:        "unrepresentable name\n",
}

# SUB/ holds two files and a further DIRECTORY. SUB/DEEP is what makes
# "C:\SUB\DEEP" a path rather than a file: the self-test walks down two
# levels and back up with a trailing separator, and an earlier fixture
# that had only SUB/DEEP.TXT turned that whole sequence into a chain of
# EPTHNF -- correct answers to the wrong question, and no coverage of a
# nested path at all. Note that SUB/DEEP and SUB/DEEP.TXT coexist on
# purpose: they are separate objects whose 8.3 names differ only by the
# extension, so a resolver that matched on the stem alone would pick the
# wrong one.
SUB_DIRS = ["SUB", "SUB/DEEP"]

SUB_FILES = {
    "SUB/INNER.TXT":      "inner file\n",
    "SUB/DEEP.TXT":       "deeper\n",
    "SUB/DEEP/BOTTOM.TXT": "bottom\n",
}

# Bigger than the negotiated 9P msize (8192), so a read that does not
# loop cannot pass. Deterministic content so the guest-side hash is
# stable across rebuilds.
#
# The NAME is what the guest self-test opens (bios/bios.c), and it has
# to be one the 8.3 mapping produces: an earlier rebuild called this
# BIGREAD.BIN, which clips to BIGREAD.BIN and so was never found, and
# the phase5-bigread golden then asserted lines the guest had stopped
# printing at all.
BIGREAD_NAME = "BIG.DAT"
BIGREAD_SIZE = 40960


def bigread_bytes():
    """The BIG.DAT contents. Also the oracle for the phase5-bigread hash:
    tools/run-all-goldens.sh recomputes FNV-1a over exactly this, so the
    golden is checked against an independent calculation rather than
    against whatever the guest happened to print."""
    return bytes((i * 7 + 13) & 0xff for i in range(BIGREAD_SIZE))


# The host-folder WRITE fixture.
#
# This is the ordinary fixture above with one directory added, and that
# directory is what turns the guest's write self-test on: bios/bios.c
# looks for C:\WTEST and does nothing at all when it is absent. The
# goldens' folder therefore never gets written to, and this one -- built
# somewhere disposable by tools/check-hostfs-write.py -- does.
#
# Everything under WTEST/ is declared here so there is a single host-side
# statement of what the guest is supposed to have written. The C in
# bios/bios.c is the independent side of the comparison.
WTEST_DIR = "WTEST"

# Already on the host when the guest boots.
WTEST_KEEP_NAME = "WTEST/KEEP.TXT"
WTEST_KEEP_BODY = b"keep me exactly as I am\n"

# Already there, and deleted by the guest.
WTEST_DEL_NAME = "WTEST/DEL.TXT"
WTEST_DEL_BODY = b"the guest deletes this one\n"

# Already there, and overwritten by the guest with something SHORTER.
# The old body is long so a create that failed to truncate leaves a tail
# behind that the size check cannot miss.
WTEST_OVER_NAME = "WTEST/OVER.TXT"
WTEST_OVER_OLD = b"old contents, and plenty of them\n" * 16
WTEST_OVER_NEW = b"overwritten\n"

# Created by the guest.
WTEST_NEW_NAME = "WTEST/NEW.TXT"
WTEST_NEW_BODY = b"hostfs write test\n"

# Written immediately AFTER the delete above -- the sequence that
# corrupted a folder under vvfat. Deliberately a different length from
# WTEST_DEL_BODY, so a file that inherited the deleted one's length
# fails on size before anything has to look at the bytes.
WTEST_AFTER_NAME = "WTEST/AFTER.TXT"
WTEST_AFTER_BODY = b"written after the delete\n"

# Created, then renamed. REN1.TXT must not be on the host afterwards.
WTEST_REN_FROM = "WTEST/REN1.TXT"
WTEST_REN_TO = "WTEST/REN2.TXT"
WTEST_REN_BODY = b"rename me\n"

# A directory the guest creates, and a file it writes inside it.
WTEST_DIR_NAME = "WTEST/NEWDIR"
WTEST_INNER_NAME = "WTEST/NEWDIR/INSIDE.TXT"
WTEST_INNER_BODY = b"inside a folder the guest made\n"

# A directory the guest creates and removes again: it must be gone.
WTEST_TMPDIR_NAME = "WTEST/TMPDIR"

# Ten times one transport frame's payload (4080 bytes), so a write that
# issued a single frame and reported the whole count is short by an
# amount no rounding could excuse.
WTEST_BIG_NAME = "WTEST/BIGW.DAT"
WTEST_BIG_SIZE = 40960


def wtest_big_bytes():
    """The BIGW.DAT contents. A different pattern from bigread_bytes()
    on purpose: a write path that somehow echoed the file the guest had
    just READ would otherwise hash correctly."""
    return bytes((i * 13 + 5) & 0xff for i in range(WTEST_BIG_SIZE))


# Everything under WTEST/ the guest creates, renames or deletes. Used by
# tools/check-hostfs-write.py to decide which pre-existing files must be
# untouched: everything NOT in this set must come out byte-identical.
WTEST_GUEST_WRITES = (
    WTEST_NEW_NAME, WTEST_OVER_NAME, WTEST_DEL_NAME, WTEST_AFTER_NAME,
    WTEST_REN_FROM, WTEST_REN_TO, WTEST_INNER_NAME, WTEST_BIG_NAME,
)


def build_hostfs_write(base):
    """The read fixture, plus the WTEST directory that arms the guest's
    write self-test. Point it at a THROWAWAY folder: the guest writes
    into it, which is the entire point, and it is rebuilt from scratch
    on every run so a pass can never rest on a previous run's leftovers.
    """
    build(base)

    wtest = os.path.join(base, WTEST_DIR)
    if os.path.isdir(wtest):
        shutil.rmtree(wtest)
    os.makedirs(wtest)

    write(os.path.join(base, WTEST_KEEP_NAME), WTEST_KEEP_BODY)
    write(os.path.join(base, WTEST_DEL_NAME), WTEST_DEL_BODY)
    write(os.path.join(base, WTEST_OVER_NAME), WTEST_OVER_OLD)

    print("host-folder write fixture rebuilt in %s" % wtest)


# The vvfat drive is a SEPARATE fixture folder, deliberately.
#
# QEMU's vvfat driver presents a host directory as a FAT16 block device,
# and the guest reaches it through the stock EmuTOS block layer -- MBR
# scan, BPB, FAT -- rather than through 9P. What it needs from a fixture
# is therefore nothing like what the host-folder goldens need: no long
# names, no pre-1980 timestamps, no 120-entry directory. Two files are
# enough, and pointing vvfat at the host-folder fixture instead would
# couple the two sets of goldens for no gain.
#
# HELLO.TXT deliberately has NO trailing newline: the guest prints its
# contents on one line and a newline in the file would split the golden
# line in two.
VVFAT_DEFAULT = "/tmp/shinogi-vvfat"

VVFAT_FILES = {
    "HELLO.TXT": "vvfat hello",
}

# What the guest's own self-test creates on this drive (bios/bios.c,
# vblk_writetest). These are outputs, not fixtures: they are swept away
# on a rebuild.
#
# The contents are declared here so there is one host-side statement of
# what the guest is supposed to have written, used by two consumers that
# would otherwise each keep their own: tools/check-vvfat-write.py
# compares the bytes that reached the host folder, and
# tools/run-all-goldens.sh recomputes the FNV-1a hash the guest prints
# after reading the file back. The C in bios/bios.c is the independent
# side of both comparisons.
VVFAT_NEW_NAME = "NEW.TXT"
VVFAT_NEW_BODY = b"vvfat write test"

# NEW.TXT does not survive under that name: the self-test renames it.
VVFAT_RENAMED_NAME = "RENAMED.TXT"

VVFAT_BIG_NAME = "BIGWRITE.DAT"
VVFAT_BIG_SIZE = 12288

# Created, written, and then deleted by the guest. It is listed as a
# guest write because the guest CREATES it -- whether it is still on the
# host afterwards is exactly what tools/check-vvfat-write.py reports on.
VVFAT_DEL_NAME = "DEL.TXT"

VVFAT_DIR_NAME = "NEWDIR"
VVFAT_INNER_NAME = "NEWDIR/INSIDE.TXT"
VVFAT_INNER_BODY = bytes((ord('a') + (i % 26)) for i in range(64))

VVFAT_GUEST_WRITES = (VVFAT_NEW_NAME, VVFAT_RENAMED_NAME, VVFAT_BIG_NAME,
                      VVFAT_DEL_NAME)


def vvfat_write_bytes():
    """The BIGWRITE.DAT contents. Larger than vvfat's 8KB cluster on
    purpose, and a different pattern from bigread_bytes() so a read-back
    that returned BIG.DAT instead could not hash correctly."""
    return bytes((i * 11 + 7) & 0xff for i in range(VVFAT_BIG_SIZE))

# BIG.DAT is the same content as the host folder's, so tools/
# run-all-goldens.sh checks both hashes against the one oracle in
# bigread_bytes(). On this drive it proves something different: 40960
# bytes is five clusters at vvfat's 16 sectors per cluster, so a FAT
# walk that repeated or skipped a cluster, or a virtio-blk request whose
# byte count wrapped, changes the hash.


def write(path, data):
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


def build(base):
    os.makedirs(base, exist_ok=True)
    for d in SUB_DIRS:
        os.makedirs(os.path.join(base, d), exist_ok=True)

    for name, body in ROOT_FILES.items():
        write(os.path.join(base, name), body)

    for name, body in SUB_FILES.items():
        write(os.path.join(base, name), body)

    write(os.path.join(base, BIGREAD_NAME), bigread_bytes())

    # Left behind by an earlier fixture generator under a name the guest
    # never opens; it would otherwise sit in the root and appear in every
    # listing golden.
    stale = os.path.join(base, "BIGREAD.BIN")
    if os.path.exists(stale):
        os.remove(stale)

    os.utime(os.path.join(base, "FIXED.TXT"), (FIXED_MTIME, FIXED_MTIME))

    # MANY/ has its own script, since its 120 entries exist purely to
    # force multi-page directory reads.
    many = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "make-many-fixture.py")
    if os.path.exists(many):
        subprocess.run([sys.executable, many, base], check=True)

    print("fixtures rebuilt in %s" % base)


def build_vvfat(base):
    os.makedirs(base, exist_ok=True)

    # The guest WRITES to this drive: its self-test creates several files
    # and a directory here on every run that attaches it. They are
    # removed rather than left, so "rebuilt" means the same folder every
    # time and a run that failed to create them cannot pass on leftovers
    # from the run before. vvfat writes 8.3 names back in lower case, so
    # both spellings are swept.
    for name in os.listdir(base):
        full = os.path.join(base, name)
        if name.upper() in VVFAT_GUEST_WRITES:
            os.remove(full)
        elif name.upper() == VVFAT_DIR_NAME and os.path.isdir(full):
            shutil.rmtree(full)

    for name, body in VVFAT_FILES.items():
        write(os.path.join(base, name), body)

    write(os.path.join(base, BIGREAD_NAME), bigread_bytes())

    print("vvfat fixtures rebuilt in %s" % base)


def main():
    targets = sys.argv[1:] or ["/tmp/shinogi-hostfs",
                               os.path.expanduser("~/shinogi-drive-c")]
    for t in targets:
        build(t)

    # The vvfat folder is not one of the host-folder targets and is not
    # interchangeable with them, so it is always rebuilt at its own
    # location rather than being driven by the arguments above.
    build_vvfat(VVFAT_DEFAULT)


if __name__ == "__main__":
    main()

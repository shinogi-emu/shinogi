#!/usr/bin/env python3
#
# Check, from the HOST, that a guest write through QEMU's vvfat driver
# lands in the host folder -- and that nothing already in that folder
# was damaged doing it.
#
# The guest-side golden (tests/golden/phase5-vvfat.expected) asserts what
# the guest saw: that Fcreate returned a handle, that Fwrite reported the
# byte count, that reading the file back gives what was written. All of
# that can be true while the host folder is wrong, because everything the
# guest reads comes back through the same vvfat mapping that took the
# write. The claim that a write WORKED is a claim about the host, and
# only this script makes it.
#
# vvfat's read-write mode is documented by QEMU as experimental, and its
# historical failure mode is not a failed write -- it is damage to the
# directory it is mapping. So the pre-existing files are hashed before
# the guest runs and re-hashed after, and any difference is a failure
# even if the new file arrived perfectly.
#
# THIS NEEDS A WRITE-ENABLED GUEST. The default build refuses writes at
# the driver, so run:
#
#   cd ~/git/emutos && make ELF=1 TOOLCHAIN_PREFIX=m68k-atari-mintelf- \
#       DEF=-DCONF_WITH_VIRTIO_BLK_WRITE=1 qemu-virt
#
# Against a default build every "OK" below turns into "was not created",
# which is the read-only driver working, not a vvfat failure.
#
# WHAT THIS SCRIPT FOUND, and why the default is off: run it twice over
# the same folder. The second run deletes a file that was on the host at
# boot and then writes a different file inside a subdirectory. The
# second file's data reaches the host correctly and its LENGTH does not
# -- the host file is truncated to the length of the deleted one, while
# the guest is told the write succeeded. Reproducible every time.
#
# Usage: tools/check-vvfat-write.py [work-dir] [--keep]
#
# The work directory is created, filled with the vvfat fixture set, and
# used for exactly one boot. It defaults to a fresh temporary directory.
# Point it somewhere disposable: the whole point of this script is that
# the folder may come back damaged.
#
# --keep uses <work-dir>/drive exactly as it already is instead of
# rebuilding it. That is how a folder with more in it than the fixture,
# or a folder a previous run already wrote to, gets tested -- overwriting
# an entry vvfat has already committed once is a different path through
# it than creating one.
#
# Exit codes:
#   0 = the file arrived intact and nothing pre-existing changed
#   1 = a check failed (details printed)
#   2 = the test could not be run at all
#
import hashlib
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.environ.get("SHINOGI_ELF",
                     os.path.expanduser("~/git/emutos/emutos-virt.elf"))
BOOT_WAIT = int(os.environ.get("BOOT_WAIT", "30"))


def load_fixtures():
    """tools/make-fixtures.py, as a module.

    It holds what the guest is supposed to have written, which is also
    what tools/run-all-goldens.sh checks the golden hash against. The
    independent side of the comparison is the C in bios/bios.c, not a
    second copy of the same constants here.
    """
    spec = importlib.util.spec_from_file_location(
        "mkfix", os.path.join(ROOT, "tools", "make-fixtures.py"))
    mkfix = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mkfix)
    return mkfix


MKFIX = load_fixtures()

NEW_NAME = MKFIX.VVFAT_NEW_NAME
NEW_BODY = MKFIX.VVFAT_NEW_BODY
RENAMED_NAME = MKFIX.VVFAT_RENAMED_NAME
BIG_NAME = MKFIX.VVFAT_BIG_NAME
BIG_BODY = MKFIX.vvfat_write_bytes()
DEL_NAME = MKFIX.VVFAT_DEL_NAME
INNER_NAME = MKFIX.VVFAT_INNER_NAME
INNER_BODY = MKFIX.VVFAT_INNER_BODY


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def snapshot(base):
    """Every file under base: relative name -> (size, sha256).

    Keyed by the UPPER-CASED name. vvfat writes a new 8.3 entry back to
    the host in lower case -- a file the guest created as NEW.TXT
    arrives as new.txt -- which is a naming convention, not damage, and
    folding the case here keeps it from being reported as either a
    missing file or an unexpected one.
    """
    out = {}
    for dirpath, _dirnames, filenames in os.walk(base):
        for name in filenames:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, base)
            out[rel.upper()] = (os.path.getsize(full), sha256(full))
    return out


def find(base, name):
    """The host path for a guest-side name, whatever case it landed in.

    Resolves one component at a time, so a name inside a directory the
    guest created ("NEWDIR/INSIDE.TXT") is found even though vvfat wrote
    both components back in lower case.
    """
    path = base
    for want in name.split("/"):
        if not os.path.isdir(path):
            return None
        for entry in os.listdir(path):
            if entry.upper() == want.upper():
                path = os.path.join(path, entry)
                break
        else:
            return None
    return path


def build_fixture(base):
    MKFIX.build_vvfat(base)


def run_guest(folder, log):
    """Boot once with the folder attached read-write, then stop.

    fat:rw: and the absence of readonly=on are two separate things and
    both are needed: the rw: prefix puts vvfat itself into read-write
    mode, readonly=on would make the block node refuse the write
    permission virtio-blk asks for.
    """
    # The log is removed, not just truncated by QEMU's own open: the
    # loop below stops as soon as the log contains the self-test's last
    # line, and a log left by a PREVIOUS run already contains it. That
    # stops QEMU before it has booted, and every check downstream then
    # reads the old run's output and the untouched folder -- a run that
    # never happened, reported as a result.
    if os.path.exists(log):
        os.remove(log)

    cmd = [
        "qemu-system-m68k", "-M", "virt", "-m", "128",
        "-kernel", ELF,
        "-device", "virtio-gpu-device",
        "-drive", "file=fat:rw:%s,format=raw,if=none,id=hostblk" % folder,
        "-device", "virtio-blk-device,drive=hostblk",
        "-display", "none",
        "-serial", "file:%s" % log,
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)

    # Stop as soon as the guest says it is done, rather than waiting out
    # the whole window: the last line the self-test prints is the
    # read-back of the large file.
    deadline = time.time() + BOOT_WAIT
    while time.time() < deadline:
        if proc.poll() is not None:
            break
        if os.path.exists(log):
            with open(log, "rb") as f:
                if b"vblk: bigreread" in f.read():
                    break
        time.sleep(0.5)

    if proc.poll() is None:
        # SIGTERM, not SIGKILL: vvfat commits its changes as the block
        # device is closed down, and killing QEMU outright would be
        # testing something other than what a real run does.
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

    return (proc.stdout.read() or b"").decode("utf-8", "replace")


def main():
    if not os.path.isfile(ELF):
        sys.stderr.write("no guest image at %s\n" % ELF)
        return 2

    args = [a for a in sys.argv[1:] if a != "--keep"]
    keep = "--keep" in sys.argv[1:]

    if args:
        work = os.path.abspath(args[0])
        os.makedirs(work, exist_ok=True)
        made_temp = False
    else:
        work = tempfile.mkdtemp(prefix="vvfat-write-")
        made_temp = True

    folder = os.path.join(work, "drive")
    if keep:
        if not os.path.isdir(folder):
            sys.stderr.write("--keep given but no folder at %s\n" % folder)
            return 2
        made_temp = False
    else:
        if os.path.isdir(folder):
            shutil.rmtree(folder)
        os.makedirs(folder)
        build_fixture(folder)

    before = snapshot(folder)
    print("baseline: %d files" % len(before))
    for name in sorted(before):
        print("  %-16s %8d  %s" % (name, before[name][0], before[name][1]))

    log = os.path.join(work, "serial.log")
    qemu_out = run_guest(folder, log)
    if qemu_out.strip():
        print("qemu said: %s" % qemu_out.strip())

    if not os.path.exists(log) or os.path.getsize(log) == 0:
        sys.stderr.write("no serial output - the guest never ran\n")
        return 2

    with open(log, "rb") as f:
        serial = f.read().decode("utf-8", "replace")

    # The guest's own return codes, so the host-side checks below can
    # tell "vvfat dropped it" from "the guest never asked".
    #
    # Under --keep the guest legitimately fails some of these: a second
    # run finds RENAMED.TXT already there and Frename returns EACCDN,
    # which is correct behaviour and must not be reported as vvfat
    # losing the rename.
    guest_rc = {}
    print("\nguest said:")
    for line in serial.splitlines():
        line = line.rstrip("\r")
        if not line.startswith("vblk:"):
            continue
        print("  " + line)
        parts = line.split()
        if len(parts) >= 4 and parts[2] == "rc":
            try:
                guest_rc[parts[1]] = int(parts[3])
            except ValueError:
                pass

    after = snapshot(folder)
    failures = []

    # The critical one: is everything that was already there still
    # there, byte for byte? Checked FIRST, because a run that damaged
    # the folder is a failure whether or not the new file arrived.
    #
    # The names the self-test writes are excluded, and only those: under
    # --keep the folder may already hold them from an earlier run, and
    # the guest rewriting or renaming its own output is the thing being
    # tested rather than damage to somebody's file.
    guest_writes = {n.upper() for n in MKFIX.VVFAT_GUEST_WRITES}
    guest_writes.add(INNER_NAME.upper())

    for name, (size, digest) in sorted(before.items()):
        if name in guest_writes:
            continue
        if name not in after:
            failures.append("pre-existing %s is GONE" % name)
        elif after[name] != (size, digest):
            failures.append(
                "pre-existing %s CHANGED: %d/%s -> %d/%s"
                % (name, size, digest[:16], after[name][0], after[name][1][:16]))

    # The files the guest wrote, on the host, with the right bytes.
    #
    # NEW.TXT is looked for under its NEW name: the self-test renames it
    # to RENAMED.TXT, so finding the original name still there would
    # mean the rename never reached the host.
    renamed = guest_rc.get("rename", 0) == 0

    wanted = [(BIG_NAME, BIG_BODY), (INNER_NAME, INNER_BODY)]
    if renamed:
        wanted.insert(0, (RENAMED_NAME, NEW_BODY))
    else:
        print("\nnote: the guest's own rename returned %d, so NEW.TXT is "
              "expected to still be there" % guest_rc.get("rename", 0))

    print()
    for name, body in wanted:
        path = find(folder, name)
        if path is None:
            failures.append("%s was not created on the host" % name)
            continue
        actual = open(path, "rb").read()
        if actual == body:
            print("OK  %s: %d bytes on the host as %s, contents match" %
                  (name, len(actual), os.path.relpath(path, folder)))
        else:
            failures.append(
                "%s differs: host has %d bytes (sha %s), expected %d (sha %s)"
                % (name, len(actual), hashlib.sha256(actual).hexdigest()[:16],
                   len(body), hashlib.sha256(body).hexdigest()[:16]))

    if renamed:
        if find(folder, NEW_NAME) is not None:
            failures.append("%s is still on the host - the rename to %s did "
                            "not reach it" % (NEW_NAME, RENAMED_NAME))
        else:
            print("OK  %s: renamed away on the host too" % NEW_NAME)

    # The delete. This is reported rather than asserted, because what it
    # measures is a property of vvfat and not of the guest: the guest's
    # own Fdelete returns 0 and a following Fsfirst reports the file
    # gone, so a run where the host still has it is a DIVERGENCE between
    # what the guest believes and what the folder holds -- the file
    # comes back on the next boot. Printed loudly either way; a silent
    # pass here would be the single most misleading line in this script.
    del_path = find(folder, DEL_NAME)
    if del_path is None:
        print("OK  %s: the guest deleted it and it is gone from the host"
              % DEL_NAME)
    else:
        print("NOTE %s: the guest deleted it, the HOST STILL HAS IT "
              "(%d bytes). Deletes do not propagate through vvfat; the "
              "file reappears on the next boot." %
              (DEL_NAME, os.path.getsize(del_path)))

    expected_new = {RENAMED_NAME.upper(), BIG_NAME.upper(),
                    INNER_NAME.upper(), DEL_NAME.upper(), NEW_NAME.upper()}
    unexpected = set(after) - set(before) - expected_new
    if unexpected:
        failures.append("unexpected new entries: %s" % sorted(unexpected))

    print("\nafter: %d files" % len(after))
    for name in sorted(after):
        print("  %-16s %8d  %s" % (name, after[name][0], after[name][1]))

    if failures:
        print("\nFAIL check-vvfat-write")
        for f in failures:
            print("  - %s" % f)
        print("\nthe folder is left at %s" % folder)
        return 1

    print("\nPASS check-vvfat-write - every file the guest wrote reached the "
          "host intact and every pre-existing file is unchanged")
    if made_temp:
        shutil.rmtree(work)
    return 0


if __name__ == "__main__":
    sys.exit(main())

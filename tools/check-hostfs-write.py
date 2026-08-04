#!/usr/bin/env python3
#
# Check, from the HOST, that a guest write to drive C lands in the host
# folder -- and that nothing already in that folder was damaged doing it.
#
# WHY THIS EXISTS, AND WHY THE GUEST'S OWN OUTPUT IS NOT ENOUGH
#
# The guest-side lines (tests/golden/phase5-write.expected) say what the
# guest was TOLD: that Fcreate returned a handle, that Fwrite reported
# the count, that reading the file back gives the bytes written. Every
# one of those can be true while the folder is wrong, because everything
# the guest reads comes back through the same transport that took the
# write. A transport that truncated, duplicated or reordered data would
# satisfy the guest and lose the file.
#
# The claim that a write WORKED is a claim about the host folder, and
# only this script makes it: it hashes the folder before the boot and
# again after, and it checks every file the guest wrote by size and
# sha256 against tools/make-fixtures.py -- a statement of the intended
# contents written independently of the C in bios/bios.c that produces
# them.
#
# THE PRE-EXISTING FILES MATTER AS MUCH AS THE NEW ONES. The failure
# that motivated all of this was not a failed write: it was a write that
# succeeded and damaged the directory it was writing into. So every file
# that was in the folder before the boot, and that the guest never
# names, must come out byte-identical -- checked first, before anything
# about the new files.
#
# HOW THE GUEST IS ARMED
#
# The write self-test in bios/bios.c runs only when C:\WTEST exists, and
# this script is what builds a folder that has one. The thirteen goldens
# use a folder without it and are therefore never written to. That is
# also why this script insists on building its own folder from scratch:
# a folder left over from a previous run would let a pass rest on files
# the current boot never created.
#
# Usage: tools/check-hostfs-write.py [work-dir]
#
#   The work directory is created, filled with the fixture set and used
#   for exactly one boot. It defaults to a fresh temporary directory,
#   which is removed on success. Point it somewhere DISPOSABLE -- never
#   at a fixture folder the goldens depend on.
#
# Exit codes:
#   0 = every file the guest wrote reached the host intact and every
#       pre-existing file is unchanged
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
QEMU = os.environ.get("QEMU", "qemu-system-m68k")
ELF = os.environ.get("SHINOGI_ELF",
                     os.path.expanduser("~/git/emutos/emutos-virt.elf"))
HOSTFSD = os.path.join(ROOT, "tools", "hostfsd", "shinogi-hostfsd")
GOLDEN = os.path.join(ROOT, "tests", "golden", "phase5-write.expected")
BOOT_WAIT = int(os.environ.get("BOOT_WAIT", "60"))


def load_fixtures():
    """tools/make-fixtures.py, as a module.

    It holds what the guest is supposed to have written. The independent
    side of the comparison is the C in bios/bios.c, not a second copy of
    the same constants here.
    """
    spec = importlib.util.spec_from_file_location(
        "mkfix", os.path.join(ROOT, "tools", "make-fixtures.py"))
    mkfix = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mkfix)
    return mkfix


MKFIX = load_fixtures()


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def snapshot(base):
    """Every file and directory under base.

    Files map to (size, sha256); directories map to None, so a directory
    that the guest replaced with a file -- or the other way round -- is a
    difference rather than something both sides ignore.

    Names are kept EXACTLY as the host spells them. Unlike vvfat, which
    writes 8.3 entries back in lower case, the helper creates a file
    under the name the guest asked for, so a case difference here would
    be a real finding and must not be folded away.
    """
    out = {}
    for dirpath, dirnames, filenames in os.walk(base):
        for name in dirnames:
            full = os.path.join(dirpath, name)
            out[os.path.relpath(full, base).replace(os.sep, "/")] = None
        for name in filenames:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, base).replace(os.sep, "/")
            out[rel] = (os.path.getsize(full), sha256_file(full))
    return out


def run_guest(folder, work, log):
    """Boot once with the folder served over virtio-serial, then stop.

    The device set matches tools/run-golden.sh exactly, 9P device and
    all, so this is the same machine the goldens run on and not a
    special one -- a write path that only worked without the other
    devices attached would be worth knowing about.
    """
    if os.path.exists(log):
        os.remove(log)

    # The socket lives in a short path of its own. A Unix socket path is
    # limited to about 100 bytes by sockaddr_un, far less than a path is
    # generally allowed, and a work directory under a long TMPDIR really
    # does reach it. Nothing is written through this path but the socket
    # itself.
    sockdir = tempfile.mkdtemp(prefix="hfsw-", dir="/tmp")
    sock = os.path.join(sockdir, "h.sock")

    cmd = [
        QEMU, "-M", "virt", "-m", "128",
        "-kernel", ELF,
        "-device", "virtio-gpu-device",
        "-fsdev", "local,id=hostfs9p,path=%s,security_model=mapped-xattr"
                  % folder,
        "-device", "virtio-9p-device,fsdev=hostfs9p,mount_tag=shinogi",
        "-chardev", "socket,id=hostfs,path=%s,server=on,wait=off" % sock,
        "-device", "virtio-serial-device",
        "-device", "virtserialport,chardev=hostfs,name=shinogi.hostfs",
        "-display", "none",
        "-serial", "file:%s" % log,
    ]
    qemu = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)

    # QEMU is the listener, so the helper cannot connect until the socket
    # exists. The guest waits a bounded moment for the port to open and
    # then gives up, so this polls rather than sleeping a fixed time.
    helper = None
    for _ in range(200):
        if os.path.exists(sock):
            break
        if qemu.poll() is not None:
            break
        time.sleep(0.05)

    if os.path.exists(sock):
        helper = subprocess.Popen(
            [HOSTFSD, "--root", folder, "--connect", sock],
            stdout=open(os.path.join(work, "hostfsd.log"), "wb"),
            stderr=subprocess.STDOUT)
    else:
        sys.stderr.write("qemu never created %s - drive C will be absent\n"
                         % sock)

    # Stop as soon as the guest says it has finished writing. Waiting the
    # whole window out would work too, but the last line is unambiguous.
    deadline = time.time() + BOOT_WAIT
    while time.time() < deadline:
        if qemu.poll() is not None:
            break
        if os.path.exists(log):
            with open(log, "rb") as f:
                if b"hostfs: wtest end" in f.read():
                    break
        time.sleep(0.5)

    # The helper is stopped FIRST and waited for. It is what actually
    # holds the files open, and reading the folder while it may still
    # have a write in flight is how a race gets reported as a failure.
    if helper is not None:
        helper.terminate()
        try:
            helper.wait(timeout=10)
        except subprocess.TimeoutExpired:
            helper.kill()
            helper.wait()

    if qemu.poll() is None:
        qemu.terminate()
        try:
            qemu.wait(timeout=15)
        except subprocess.TimeoutExpired:
            qemu.kill()
            qemu.wait()

    shutil.rmtree(sockdir, ignore_errors=True)

    return (qemu.stdout.read() or b"").decode("utf-8", "replace")


def check_golden(lines, failures):
    """Compare the guest's own lines with tests/golden/phase5-write.expected.

    This golden is not in tools/run-all-goldens.sh and cannot be: it
    needs a folder with a WTEST directory in it, which is the one thing
    the goldens' fixture folder must never have. It is checked here
    instead, so the guest-side return codes are still pinned down
    somewhere rather than merely printed.
    """
    if not os.path.exists(GOLDEN):
        failures.append("no golden file at %s" % GOLDEN)
        return

    expected = open(GOLDEN).read().splitlines()

    # The golden carries one line no human can judge by eye: the FNV-1a
    # the guest computed over the large file it wrote and read back. It
    # is checked against the same hash computed here from the fixture
    # definition -- an independent calculation, not a re-recording of
    # whatever the guest last printed. Without this the golden would
    # agree with a guest that hashed the wrong bytes consistently.
    body = MKFIX.wtest_big_bytes()
    h = 2166136261
    for b in body:
        h ^= b
        h = (h * 16777619) & 0xffffffff
    want = "hostfs: wtest big reread %d bytes fnv1a 0x%08x" % (len(body), h)
    if want not in expected:
        failures.append("the golden's hash line does not match the fixture: "
                        "expected %r" % want)
    if lines == expected:
        print("OK  guest lines match %s" % os.path.relpath(GOLDEN, ROOT))
        return

    failures.append("the guest's own lines differ from the golden")
    import difflib
    for line in difflib.unified_diff(expected, lines,
                                     "expected", "actual", lineterm=""):
        print("  " + line)


def main():
    if not os.path.isfile(ELF):
        sys.stderr.write("no guest image at %s\n" % ELF)
        return 2

    if not os.path.isfile(HOSTFSD):
        subprocess.run(["make", "-s", "-C",
                        os.path.join(ROOT, "tools", "hostfsd")], check=False)
    if not os.path.isfile(HOSTFSD):
        sys.stderr.write("cannot build %s\n" % HOSTFSD)
        return 2

    made_temp = False
    if len(sys.argv) > 1:
        work = os.path.abspath(sys.argv[1])
        os.makedirs(work, exist_ok=True)
    else:
        work = tempfile.mkdtemp(prefix="hostfs-write-")
        made_temp = True

    folder = os.path.join(work, "drive")

    # Always rebuilt. A folder left from a previous run already holds
    # every file this script looks for, and a boot that wrote nothing at
    # all would then pass.
    if os.path.isdir(folder):
        shutil.rmtree(folder)
    os.makedirs(folder)
    MKFIX.build_hostfs_write(folder)

    before = snapshot(folder)
    print("baseline: %d entries" % len(before))

    log = os.path.join(work, "serial.log")
    qemu_out = run_guest(folder, work, log)
    if qemu_out.strip():
        print("qemu said: %s" % qemu_out.strip())

    if not os.path.exists(log) or os.path.getsize(log) == 0:
        sys.stderr.write("no serial output - the guest never ran\n")
        return 2

    with open(log, "rb") as f:
        serial = f.read().decode("utf-8", "replace")

    guest_lines = []
    for line in serial.splitlines():
        line = line.rstrip("\r")
        if line.startswith("hostfs: wtest "):
            guest_lines.append(line)

    print("\nguest said:")
    for line in guest_lines:
        print("  " + line)

    if not guest_lines:
        sys.stderr.write("\nthe guest ran no write self-test at all - is "
                         "C:\\WTEST in the fixture, and is drive C up?\n")
        return 2

    failures = []
    check_golden(guest_lines, failures)

    after = snapshot(folder)

    # ------------------------------------------------------------------
    # First: is everything the guest never named still exactly as it was?
    # ------------------------------------------------------------------
    touched = set(MKFIX.WTEST_GUEST_WRITES)
    touched.add(MKFIX.WTEST_DIR_NAME)
    touched.add(MKFIX.WTEST_TMPDIR_NAME)

    for name, meta in sorted(before.items()):
        if name in touched:
            continue
        if name not in after:
            failures.append("pre-existing %s is GONE" % name)
        elif after[name] != meta:
            failures.append("pre-existing %s CHANGED: %s -> %s"
                            % (name, meta, after[name]))
    print("\nOK  %d pre-existing entries the guest never named are unchanged"
          % len([n for n in before if n not in touched]))

    # ------------------------------------------------------------------
    # Then: every file the guest wrote, by size and hash.
    # ------------------------------------------------------------------
    wanted = [
        (MKFIX.WTEST_NEW_NAME, MKFIX.WTEST_NEW_BODY, "created and written"),
        (MKFIX.WTEST_BIG_NAME, MKFIX.wtest_big_bytes(),
         "written in chunks larger than one transport frame"),
        (MKFIX.WTEST_OVER_NAME, MKFIX.WTEST_OVER_NEW,
         "overwritten and truncated"),
        (MKFIX.WTEST_AFTER_NAME, MKFIX.WTEST_AFTER_BODY,
         "written straight after a delete"),
        (MKFIX.WTEST_REN_TO, MKFIX.WTEST_REN_BODY, "renamed into place"),
        (MKFIX.WTEST_INNER_NAME, MKFIX.WTEST_INNER_BODY,
         "written inside a folder the guest created"),
    ]

    print()
    for name, body, what in wanted:
        path = os.path.join(folder, name)
        if not os.path.isfile(path):
            failures.append("%s is not on the host (%s)" % (name, what))
            continue
        actual = open(path, "rb").read()
        if actual == body:
            print("OK  %-22s %6d bytes  %s  (%s)"
                  % (name, len(actual), sha256(actual)[:16], what))
        else:
            failures.append(
                "%s differs: host has %d bytes (sha %s), expected %d (sha %s)"
                % (name, len(actual), sha256(actual)[:16],
                   len(body), sha256(body)[:16]))

    # ------------------------------------------------------------------
    # And the things that must NOT be there.
    # ------------------------------------------------------------------
    for name, what in ((MKFIX.WTEST_DEL_NAME, "deleted by the guest"),
                       (MKFIX.WTEST_REN_FROM, "renamed away by the guest"),
                       (MKFIX.WTEST_TMPDIR_NAME, "removed by the guest")):
        if os.path.exists(os.path.join(folder, name)):
            failures.append("%s is STILL on the host - it was %s"
                            % (name, what))
        else:
            print("OK  %-22s gone from the host (%s)" % (name, what))

    if os.path.isdir(os.path.join(folder, MKFIX.WTEST_DIR_NAME)):
        print("OK  %-22s is a directory on the host" % MKFIX.WTEST_DIR_NAME)
    else:
        failures.append("%s was not created as a directory"
                        % MKFIX.WTEST_DIR_NAME)

    # Anything the guest made that nobody asked for.
    unexpected = set(after) - set(before) - touched
    if unexpected:
        failures.append("unexpected new entries: %s" % sorted(unexpected))

    # Containment, measured rather than reported.
    #
    # The guest tries to create, mkdir and delete OUTSIDE the served
    # folder, and says it was refused. That claim is worth exactly as
    # much as the folder's parent directory, which is checked here: the
    # work directory holds the drive and two logs and must hold nothing
    # else, whatever the guest was told.
    allowed = {"drive", "serial.log", "hostfsd.log"}
    strays = set(os.listdir(work)) - allowed
    if strays:
        failures.append("something was written OUTSIDE the served folder: %s"
                        % sorted(strays))
    else:
        print("OK  %-22s nothing was written outside the served folder"
              % "containment")

    if failures:
        print("\nFAIL check-hostfs-write")
        for f in failures:
            print("  - %s" % f)
        print("\nthe folder is left at %s" % folder)
        return 1

    print("\nPASS check-hostfs-write - every file the guest wrote reached "
          "the host intact and every pre-existing file is unchanged")
    if made_temp:
        shutil.rmtree(work)
    return 0


if __name__ == "__main__":
    sys.exit(main())

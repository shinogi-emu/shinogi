Drive integration checks
========================

Compile probe.c with m68k-atari-mintelf-gcc -m68020-60 -O2 -Wall -Wextra
-Werror -o /tmp/drives.prg. Run check.py with --firmware <EmuTOS ELF>,
--guest <matching drive C tree>, --probe /tmp/drives.prg and --qemu <the
maintained native QEMU>. --work may name a fresh disposable output folder.
The script creates all fixtures; it never attaches the supplied guest tree
writably. Requires mkfs.fat, mtools and fsck.fat.

Each boot runs the probe from AUTO under EmuTOS and again from MINT.CNF
under FreeMiNT. It checks scrambled named folder ports, separate current
paths, overlapping helper handles, interleaved DTAs, cross-drive rename
refusal, multi-frame writes/overwrite/rename/delete/directories, and the
historical delete-short-then-write-long regression. The host checks exact
bytes, untouched files, readonly folder contents/modes and image hashes,
and runs fsck.fat after QMP quit. A second boot must read prior writes.
A third boot swaps both floppy images and geometries (720KB/1.44MB), and
checks BIOS sector routing and physical write protection on A/B.

Compile all-folders.c with the same flags; check-all-folders.py takes the
same required arguments and checks all23 production folder mappings in
both EmuTOS and FreeMiNT. U is reserved by FreeMiNT. Port order is reversed
so C is the last channel and the highest port is actually exercised.

The Windows tests are in tests/win-settings/test_drives.c (real editor and
INI/path validation) and test_editions.c (actual installer side-by-side
lifecycle). The latter takes three pairs of edition name and installer
absolute path and requires a disposable Windows profile. It installs all
three, upgrades/uninstalls the first, and verifies the others and all guest
files remain. Use an isolated Wine/Xvfb profile for automated runs; actual
Windows laptop smoke testing remains separate.

Compile failover.c with the same guest flags. check-failover.py takes the
same required arguments and kills D's helper while C/D/Z files are open.
It requires bounded failure on D, continued C/Z reads, safe closure of an
overlapping helper handle, persistent drive ownership and unchanged files.

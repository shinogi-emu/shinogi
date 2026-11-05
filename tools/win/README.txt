@EDITION@ - Atari GEM on QEMU (Windows)
=====================================

A complete, self-contained package: the guest, the emulator and the
launcher all ship together. Nothing else needs installing.

This is @EDITION@. CPU editions install side by side, each with its own
program folder, desktop shortcut, Start menu folder, settings and default
boot drive. Upgrading or uninstalling one leaves the others installed.
The optional LAN bridge and signed TAP driver are shared across editions.

    shinogi.exe          launch it
    shinogi-hostfsd.exe  serves drive C, started for you
    emutos-virt.elf      the guest (EmuTOS), 1024x768 truecolor by default
    qemu\                the bundled QEMU for Windows
    BUILD.txt            exact CPU and QEMU version in this build
    sdl-grab-probe.exe   diagnostic, see below


Drive C
-------

Drive C: is a folder on your host:

    %USERPROFILE%\@EDITION@\drive-c

The installer asks for this location. Choose your existing drive C, or an
empty folder to receive the bundled system. Choose the folder containing
AUTO and MINT, rather than the parent folder of an extracted zip.

Put files there and they appear on the Atari desktop; the 8.3 names you
see are what the mapping gives them.

Open Shinogi Settings from the Start menu to change the drive C folder,
start resolution or "Start full screen". Changes apply on the next start.
Choosing another folder does not copy or move your files. The drive C
shortcuts always open the currently selected folder.

Additional drives opens a separate editor. Choose a slot, browse for its
folder or image, select Read-only if wanted, and press OK then Save.
Switching slots retains edits. Remove mapping disconnects that slot after
the next restart; it does not delete the folder or image.

Folder drives D: through Z: (except FreeMiNT's reserved U:) have fixed letters, independent of discovery
order. Each uses a separate confined helper. Read-only mappings refuse all
writes and attribute changes, including startup attribute repair.

Up to four raw hard-disk images (.img/.hdf) may be attached. They must be
formatted already; FAT12/FAT16 and Atari/DOS partition tables are supported.
Partitions take available drive letters, skipping C: and mapped folders.
Images default to read-only; clear that option to enable persistent writes.
The same image cannot occupy two slots. QEMU also locks writable images
against another emulator instance. Host folders are never writable vvfat.

Floppy A: and B: accept formatted raw .st/.img images, 720 KB or 1.44 MB
with 512-byte sectors. Read-only defaults on. Attachment, removal and swaps
require a restart so BIOS/BPB and filesystem caches are rebuilt. Live eject,
MSA/STX images and floppy-controller/protected-disk emulation are not provided.

Older shared installations and %USERPROFILE%\shinogi-drive-c are kept.
New editions start with separate boot folders. Browse to an existing drive
only if you intend to share it; separate writable boot folders let editions
run together without changing each other's system files.

Toggle full screen with left Ctrl + left Alt + F in the emulator window.
Open Settings with Ctrl + Alt + S while the QEMU window is focused.
This shortcut applies only to the running emulator. SDL keeps host keyboard
shortcuts available, including Ctrl+Alt+S and Alt+Tab. The guest keeps running;
saved settings apply to the next launch. Press it again to bring an existing
Settings window to the front.

The display checks at 50 Hz during input and drawing, converting only changed
screen regions. Still screens use fewer checks and need no repeated transfers.
Normal starts skip filesystem regression tests and verbose directory tracing;
the welcome screen pauses for one second and Shift can keep it displayed.
Directory replies are batched when both the firmware and helper support it.
The resolution is stored as res = WIDTHxHEIGHT in drive C's SHINOGI.INI,
so the guest's resolution picker and Windows settings use the same value.
The drive C location and full-screen choice are stored per Windows user
in %APPDATA%\@EDITION@\settings.ini. Uninstalling keeps these settings and
the drive C folder. Command-line resolution overrides still work.

If the folder is missing, the launcher opens Settings so you can locate
it. Startup errors show the selected folder and helper diagnostic, or
show the last QEMU diagnostic with its full log path for a QEMU failure.
The QEMU log is replaced on each start.

Sound support is planned for a future release. The launcher starts without
audio so that unsupported sound options cannot prevent Atari from booting.

It is READ/WRITE. The guest can create, write, delete, rename files and
make directories, and the changes appear in the folder immediately.

KEEP THE NAMES SHORT - 8 characters plus a 3 character extension. A
longer name does not fail cleanly, it opens the WRONG FILE. A basename
over eight characters is clipped to eight, and the clipped name then
opens whatever file has it: "HighWire_logo.png" returns the file called
"HighWire.png", and so does "HighWire_logo2.png", which names nothing at
all. Three names, one file, and no error reported anywhere.

A four character extension is the other half of it, and that half at
least announces itself - "resolv.conf" is listed as "resolv.con" and
then cannot be opened by any name.

For a picture the first case is a visible surprise. For a settings file
or a saved document it looks exactly like success and can go unnoticed
for a long time.

How it works, and why it changed twice: QEMU cannot build virtio-9p on
Windows at all, so the first Windows builds had no drive C. The next
used QEMU's vvfat, which maps a directory as a FAT disk -- but its
write mode corrupts silently (delete a file, save a document, and the
document is truncated to the deleted file's length). So the drive is now
served by a small helper process, shinogi-hostfsd.exe, installed beside
shinogi.exe and started and stopped for you. The guest talks to it over
a virtio-serial pipe, which every QEMU build has.

Writes are verified against the host folder rather than by asking the
guest: files are checked by size and SHA-256, pre-existing files are
checked to be untouched, and the folder's parent is checked to be sure
nothing was written outside it.


Networking
----------

Internet (NAT) is the default and needs no driver or Windows network setup.
Optional LAN mode makes Atari reachable from other machines on your network,
including incoming ping and server connections.

In Shinogi Settings, choose LAN (Ethernet), select your Ethernet adapter and
choose Set up LAN. Approve the Windows administrator prompt. Setup installs
the bundled signed TAP-Windows6 driver and creates a dedicated Windows bridge;
your Ethernet connection may disconnect briefly. Atari then obtains its
address, router and DNS automatically using DHCP. Save Settings and restart
Shinogi. Advanced offers static Atari addressing when needed.
Settings also shows the last Atari LAN address reported by DHCP, so you can
see what address to ping. This is explicitly the last known address, not a
live connection check.

Automatic setup currently supports a connected physical Ethernet adapter with
Windows addressing set to automatic (DHCP). Wi-Fi, static Windows host
addressing and unrelated existing Windows bridges are not changed by setup.
Internet mode remains available on those machines. To change the Ethernet
adapter after setup, choose Remove LAN, select the new adapter, then Set up
LAN. Returning to Internet mode keeps the bridge available for later reuse;
Remove LAN restores Ethernet, and uninstall also removes the managed bridge.
The shared TAP driver and dedicated adapter are kept for other installations.

Host setup happens only when you choose Set up LAN or Remove LAN. Launching
in LAN mode checks that the selected adapter and managed bridge still exist;
it reports a problem instead of silently changing to Internet mode.
Setup diagnostics are in %PROGRAMDATA%\shinogi\lan-setup.log.

LAN setup preserves a backup of the original guest MINT.CNF as SHNNET.BAK
before adding its network hook. Later mode changes update only SHNNET.SH.
Custom guest network commands are not overwritten. The networking files keep
8.3 names and the DHCP resolver configuration lives in the guest ramfs.
Driver/client licenses, provenance and matching original source packages are
installed in the network\ directory beside the launcher.

Firmware updates
----------------

Normal starts use the EmuTOS image bundled with the installed Shinogi build,
including after an upgrade. For a deliberate custom firmware override, put it
in the selected drive C folder as CUSTOM.IMG. Older EMUTOS.IMG and EMUTOS.ELF
files are preserved but no longer override the installed firmware. If you
previously used EMUTOS.IMG as an intentional override, copy it to CUSTOM.IMG.
Remove CUSTOM.IMG to return to the bundled image.


Running
-------

Double-click shinogi.exe, or use the Start Menu / Desktop shortcut.

You should get an Atari GEM desktop at the chosen resolution (1024x768 by default). The mouse pointer tracks
your host pointer directly - there is no pointer grab and no need to
press anything to release it - and typing goes to the guest.

Logs are written to:

    %LOCALAPPDATA%\@EDITION@\shinogi-serial.log
    %LOCALAPPDATA%\@EDITION@\shinogi-guest-errors.log

(not next to the program, which may be read-only when installed under
Program Files).


Guest system updates
--------------------

Both CPU editions install the bundled VIRTIONE.XIF in the selected drive C's
FreeMiNT tree. BUILD.txt records the kernel, firmware and network driver
checksums used by this installer.

Each installer also updates AUTO\MINT.PRG with the matching native
FreeMiNT kernel. Installing the other edition switches the same drive C
between the 68040 and 68060 kernels; personal files elsewhere in drive C
are left alone.

The 68060 edition also installs 060SP.PRG first in the AUTO folder. A
real 68060 moved a small group of older integer and floating-point
instructions into software; this package supplies that compatibility to
existing Atari applications while the emulator retains strict 68060
exception behaviour. The program contains the complete Motorola licence
notice under which the software package may be redistributed.


The display backend
-------------------

shinogi.exe uses QEMU's SDL display, the same as the Linux and macOS
launchers, so all three behave alike. SDL is confirmed working on
Windows: the pointer tracks correctly and there is no grab.

The bundled Windows emulator includes SDL. Its window opens at the selected
resolution and can be resized by dragging it. Open Settings with Ctrl+Alt+S
to select the resolution for the next start.

The reason the GTK option is there at all: on some Linux hosts SDL is
unusable with this guest. With an absolute pointing device - which is
what gives you a pointer tracking yours exactly, rather than a captured
relative mouse - SDL grabs the pointer the moment it moves inside a
*focused* window, and only releases the grab when the pointer touches a
window edge. Where the grab also stops mouse motion being delivered, the
edge can never be reached, the grab never lifts, and the guest pointer
is frozen until the window loses focus. That happens on Linux under a
GNOME Remote Login session. It does NOT happen on Windows.

If a machine ever shows that symptom - pointer dead while the window is
focused, fine while it is not - use gtk there, and confirm it with:

    sdl-grab-probe.exe

A small SDL2 program that tests the same thing with no QEMU and no
guest involved. Click the window it opens and keep the mouse moving over
it. It toggles the pointer grab every three seconds and prints how many
mouse-motion events arrived in each interval.

Healthy host - similar counts either way:

    grab=OFF focus=1  motion events in last 3s: 341
    grab=ON  focus=1  motion events in last 3s: 336

Affected host - zero whenever grabbed:

    grab=OFF focus=1  motion events in last 3s: 337
    grab=ON  focus=1  motion events in last 3s: 0

For reference the Linux box in the remote session reports 337 and 302
ungrabbed, 0 and 0 grabbed.


Known cosmetic issue
--------------------

On the Linux remote-desktop session the host mouse pointer stays visible
on top of the guest window, so two pointers are drawn. QEMU is already
asking for the host pointer to be hidden and the remote-desktop layer
ignores it. This has not been seen on Windows.


Licensing / provenance
----------------------

The qemu\ directory contains the QEMU build selected when this package
was assembled. BUILD.txt records its version and the emulated CPU. Some
Shinogi builds carry the Atari DMA-sound device and m68k CPU correctness
fixes required by the guest; development CPU editions can also contain
unreleased CPU work and are marked as private test builds there.

QEMU is free software under the GNU GPL version 2; its licence texts are
in qemu\COPYING and qemu\COPYING.LIB, and upstream source is available
from https://www.qemu.org/. Only the files needed for the m68k target are
included; the other architecture binaries and firmware images are
omitted.

EmuTOS is free software under the GNU GPL version 2, from
https://emutos.sourceforge.io/.

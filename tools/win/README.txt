shinogi - Atari GEM on QEMU (Windows)
=====================================

A complete, self-contained package: the guest, the emulator and the
launcher all ship together. Nothing else needs installing.

The two CPU editions are named Shinogi-040 and Shinogi-060. Installing one
switches the shared program tree and drive C to that edition; it does not
remove personal files or settings in drive C.

    shinogi.exe          launch it
    shinogi-hostfsd.exe  serves drive C, started for you
    emutos-virt.elf      the guest (EmuTOS), 1280x720 truecolor
    qemu\                the bundled QEMU for Windows
    BUILD.txt            exact CPU and QEMU version in this build
    sdl-grab-probe.exe   diagnostic, see below


Drive C
-------

Drive C: is a folder on your host:

    %USERPROFILE%\shinogi-drive-c

It is created on first run. Put files there and they appear on the Atari
desktop; the 8.3 names you see are what the mapping gives them.

It is READ/WRITE. The guest can create, write, delete, rename files and
make directories, and the changes appear in the folder immediately.

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


Running
-------

Double-click shinogi.exe, or use the Start Menu / Desktop shortcut.

You should get a 1280x720 Atari GEM desktop. The mouse pointer tracks
your host pointer directly - there is no pointer grab and no need to
press anything to release it - and typing goes to the guest.

Logs are written to:

    %LOCALAPPDATA%\shinogi\shinogi-serial.log
    %LOCALAPPDATA%\shinogi\shinogi-guest-errors.log

(not next to the program, which may be read-only when installed under
Program Files).


Networking
----------

The guest uses an IPv4 NAT connection requiring no Windows network
driver, administrator setup or firewall exception. Outbound TCP, UDP,
DNS and ping to the virtual gateway work; other machines on the LAN
cannot initiate connections to the guest.

Both the 68040 and 68060 editions include an updated VIRTIONE.XIF. The
installer puts it in the existing FreeMiNT tree under shinogi-drive-c
automatically. Its 20 ms receive polling avoids the roughly one-second
latency caused by FreeMiNT's generic slow network timer.

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

You can switch to GTK with:

    shinogi.exe gtk

or the "shinogi (GTK display)" Start Menu shortcut. GTK adds a menubar
and opens a larger window (the launcher passes zoom-to-fit=off and
scale=1.5, neither of which SDL supports - grab-mod is SDL's only
sub-option). On SDL the window opens at 1280x720 and can be resized by
dragging it.

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

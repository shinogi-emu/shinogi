shinogi - Atari GEM on QEMU (Windows)
=====================================

A complete, self-contained package: the guest, the emulator and the
launcher all ship together. Nothing else needs installing.

    shinogi.exe          launch it
    shinogi-hostfsd.exe  serves drive C, started for you
    emutos-virt.elf      the guest (EmuTOS), 1280x720 truecolor
    qemu\                the bundled QEMU 11.0.92 for Windows
    sdl-grab-probe.exe   diagnostic, see below


Drive C
-------

Drive C: is a folder on your host:

    %USERPROFILE%\shinogi-drive-c

It is created on first run. Put files there and they appear on the Atari
desktop; the 8.3 names you see are what FAT gives them.

Drive C is back for Windows, and it is now the SAME mechanism the Linux
and macOS builds use. It is not QEMU's vvfat driver any more, and it is
not 9p either -- QEMU cannot build 9p on Windows at all.

What serves it is shinogi-hostfsd.exe, a small program in this
directory. The launcher starts it before the emulator and stops it
afterwards; you never run it yourself and it has no window. It talks to
the guest over a virtio-serial port, which is just a byte pipe, and does
the real work of opening and reading your files. The guest never learns
a host path.

The practical difference from b4: the folder is live. Add, rename or
delete a file on the host and the guest sees it on its next look, with
no drive to re-attach and no image to rebuild.

Writing: not yet, and honestly. The host end of the link implements the
whole write set -- write, create, delete, rename, make and remove
directory -- but the guest's own filesystem layer still answers Fwrite
with "access denied" until its write path is finished, so from inside
the Atari desktop drive C is read-only. That is one change away, on the
guest side, and nothing in this package will have to change with it.

What this replaces is worth stating plainly, because it is the reason
the drive was read-only before. b4 used QEMU's vvfat driver, whose
read-write mode LOSES HOST DATA: delete a file on the drive, then write
a different file in a subdirectory, and the host file is truncated to
the deleted one's length while the guest is told the write succeeded.
That is somebody's document silently becoming eight bytes long, and it
is measured rather than assumed. Nothing in the new path can do it:
every operation is an explicit request that either completed or did not.

If drive C does not appear, the launcher will tell you so and start the
emulator without it. The helper's own log is:

    %LOCALAPPDATA%\shinogi\shinogi-hostfsd.log

Drive C needs Unix-domain socket support, which Windows has had since
Windows 10 version 1803 (April 2018) -- the same era of Windows the
bundled QEMU itself needs, so there is no machine that can run one and
not the other.

Uninstalling does NOT delete %USERPROFILE%\shinogi-drive-c. Your files
are yours.


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

The qemu\ directory contains an unmodified QEMU 11.0.92 build for
Windows, taken from the official binaries at:

    https://qemu.weilnetz.de/w64/qemu-w64-setup-20260729.exe
    sha256 f88141ccb5597ceb7bed58ffb6cd173d3fc14233772bc6edff6583c7b4bb816c

QEMU is free software under the GNU GPL version 2; its licence texts are
in qemu\COPYING and qemu\COPYING.LIB, and its source is available from
https://www.qemu.org/. It is redistributed here as-is - nothing in QEMU
has been patched. Only the files needed for the m68k target are
included; the other architecture binaries and firmware images from the
upstream package are omitted.

EmuTOS is free software under the GNU GPL version 2, from
https://emutos.sourceforge.io/.

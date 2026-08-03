shinogi - Atari GEM on QEMU (Windows)
=====================================

A complete, self-contained package: the guest, the emulator and the
launcher all ship together. Nothing else needs installing.

    shinogi.exe          launch it
    emutos-virt.elf      the guest (EmuTOS), 1280x720 truecolor
    qemu\                the bundled QEMU 11.0.92 for Windows
    sdl-grab-probe.exe   diagnostic, see below


Drive C
-------

Drive C: is a folder on your host:

    %USERPROFILE%\shinogi-drive-c

It is created on first run. Put files there and they appear on the Atari
desktop; the 8.3 names you see are what FAT gives them.

It is READ-ONLY for now. The guest can list and open and run what is
there, and cannot write back. That is deliberate: the mechanism
underneath is QEMU's vvfat driver, whose read-write mode is documented
as experimental with a history of corrupting the directory it is mapping,
so writing is being evaluated separately rather than switched on and
hoped for. Change files from the host side meanwhile.

Note this is NOT the same mechanism the Linux build has used until now.
QEMU cannot build virtio-9p on Windows at all -- its meson.build requires
the host to be Linux, macOS or FreeBSD -- so the Windows binary carries
the device name with none of the implementation behind it. vvfat is
present in every build, and EmuTOS reads the DOS MBR and FAT16 it
synthesises using its own stock filesystem code.


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

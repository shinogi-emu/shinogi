shinogi - Atari GEM on QEMU (Windows)
=====================================

A complete, self-contained package: the guest, the emulator and the
launcher all ship together. Nothing else needs installing.

    shinogi.exe          launch it
    emutos-virt.elf      the guest (EmuTOS), 1280x720 truecolor
    qemu\                the bundled QEMU 11.0.92 for Windows
    sdl-grab-probe.exe   diagnostic, see below


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

shinogi.exe uses QEMU's gtk display. That is deliberate.

With an absolute pointing device - which is what gives you a pointer
that tracks yours exactly, instead of a captured relative mouse - QEMU's
SDL frontend grabs the pointer the moment it moves inside a *focused*
window, and only releases the grab when the pointer touches a window
edge. On a host where that grab also stops mouse motion being delivered,
the edge can never be reached, so the grab never lifts and the guest
pointer is frozen until the window loses focus. That is exactly what
happens on Linux under a GNOME Remote Login session.

GTK never grabs while the pointing device is absolute, and actively
releases the grab if a device becomes absolute, so it does the right
thing. macOS/cocoa is fine too. There is no way to switch the SDL
behaviour off: grab-mod is its only sub-option, and nothing on the guest
side can prevent it without giving up absolute positioning.

Whether Windows is affected at all is still an open question. It very
likely is not, because the Linux failure comes from how the X11/XWayland
pointer grab behaves in a remote session and Windows uses an entirely
different mechanism. Two ways to find out:


1. Run shinogi.exe with an argument:

       shinogi.exe sdl

   Same guest, forced onto the SDL display. Focus the window and move
   the mouse.

     - pointer tracks normally
           -> Windows is unaffected, either backend works
     - pointer frozen while focused, but tracks while the window is NOT
       focused
           -> Windows has the same fault, and gtk matters everywhere


2. Run sdl-grab-probe.exe from a command prompt.

   A small SDL2 program that tests the same thing with no QEMU and no
   guest involved. Click the window it opens and keep the mouse moving
   over it. It toggles the pointer grab every three seconds and prints
   how many mouse-motion events arrived in each interval.

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
ignores it. Windows should not do this. If it does, that is worth
knowing, because it would mean the cause is something other than the
remote session.


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

Shinogi - Linux bundle
======================

A complete Atari machine in one directory: the emulator, its libraries,
the guest ROM image and a ready-to-run drive C. Nothing is installed and
nothing outside this directory is needed to start it.

    tar xzf Shinogi-060-<version>-linux-x86_64.tar.gz
    cd Shinogi-060-<version>-linux-x86_64
    ./shinogi doctor        # check the bundle against this machine
    ./shinogi run           # boot with a window

The guest takes roughly a minute to reach the GEM desktop.


Headless and scripted use
-------------------------

The bundle is built to be driven without a display, which is how it is
tested and how it is meant to be used over a plain SSH session or inside
WSL:

    ./shinogi start                     # boot in the background
    ./shinogi wait-idle                 # until the screen stops changing
    ./shinogi log -f                    # watch the serial console
    ./shinogi screenshot desktop.png    # capture the screen
    ./shinogi stop

"shinogi screenshot" writes a real PNG of the guest's screen whether or
not there is a display attached, so a boot can be verified from a
terminal. "shinogi log" is the guest's serial console, which carries the
kernel's own messages and is usually where a failed boot explains itself.

"shinogi monitor <command>" passes a command straight to the emulator's
monitor if you need something the launcher does not wrap - "info block",
"info network", "stop", "cont", and so on. "shinogi qmp <json>" is the
same for the machine-readable interface.


Working the machine without a display
-------------------------------------

The pointer and the keyboard are driven from the command line, in screen
pixels, whether or not anything is on screen:

    ./shinogi move 640 360              # put the pointer somewhere
    ./shinogi click 40 40               # move and click in one step
    ./shinogi dclick 40 40              # open the icon under 40,40
    ./shinogi click --right 500 300
    ./shinogi drag 200 100 600 400      # with the motion in between
    ./shinogi key ret                   # ret, esc, alt-x, shift-a, f1...
    ./shinogi type "http://example.com"

Coordinates are pixels with 0,0 at the top left, in the resolution the
bundle boots at - 1280x720 unless SHINOGI_XRES/SHINOGI_YRES say
otherwise. The guest's pointing device is a tablet, so a coordinate is
where the pointer goes, not how far it moves: there is no accumulated
drift and no need to home the pointer first.

The loop that works is: do something, wait for the screen to settle,
look at it.

    ./shinogi dclick 40 40
    ./shinogi wait-idle
    ./shinogi screenshot win.png --crop 0,0,640,400 --scale 2

"wait-idle" returns as soon as two captures in a row are identical, so
there is nothing to guess. Sleeping instead is what produces screenshots
of half-drawn windows, and a half-drawn window read as a rendering fault
is a wasted afternoon.

Finding coordinates is done by looking. Capture the screen, read the
pixel position off the image, click there. GEM menus drop on hover, so
moving the pointer onto a menu title and capturing is enough to see a
menu and its keyboard shortcuts without clicking anything:

    ./shinogi move 96 7
    ./shinogi screenshot menu.png --crop 60,0,360,300

Button transitions are paced 40 ms apart, because the guest samples its
mouse once a frame and cannot see anything faster. SHINOGI_CLICK_MS
changes that if some guest wants a different rhythm. This is worth
knowing because getting it wrong fails quietly: every event is accepted,
single clicks still work, and only double clicks and drags go missing.

--crop and --scale exist for the same reason. GEM type is small, and the
whole screen shrunk to fit is unreadable in a way that invents faults
rather than hiding them - this project has chased two rendering findings
that were only ever artefacts of a downscaled screenshot. Cut out the
part that matters and magnify it by a whole number instead: every pixel
becomes a block of identical pixels, so nothing appears that was not
there.


Drive C
-------

The guest's drive C is an ordinary host directory, created on first start
at ~/.shinogi/drive-c from the pristine copy inside the bundle. Put
Atari programs there and they appear on C: inside the guest. Print the
path with:

    ./shinogi drive-c

If the guest's C: gets into a state you would rather abandon:

    ./shinogi reset-drive-c

which moves the old one aside to drive-c.old and lays down the shipped
tree again.

The AUTO folder runs in name order, which is what it needs: the 68060
support package before the display driver before the kernel. The folder
is served by a helper that sorts, so the order does not depend on how the
host filesystem happens to enumerate the directory - a detail that is not
cosmetic, since the wrong order panics the guest during boot.

Filenames on drive C are 8.3. A four-character extension cannot be
spelled there at all - a file written as "resolv.conf" is listed as
"resolv.con" and then cannot be opened by any name. This bites configuration
files most; /etc inside the guest is built on a long-name filesystem for
exactly this reason.


Networking
----------

The guest gets a NAT'd connection with no setup and no privileges. It is
not a host on your LAN and nothing can connect in to it, but outbound
connections and name resolution work from anywhere.


Where things are
----------------

    ./shinogi                bundle launcher
    ./bin/                   the emulator and its helpers
    ./lib/                   its libraries
    ./lib/glibc/             the C library and the loader, kept separate
    ./guest/emutos-virt.elf  the ROM image
    ./guest/drive-c/         the pristine drive C
    ~/.shinogi/drive-c       the drive C the guest actually uses
    ~/.shinogi/logs/         serial console, guest errors, emulator output,
                             drive C helper output
    ~/.shinogi/run/          pid file and monitor socket

Set SHINOGI_HOME to move all of the runtime state somewhere else - two
bundles pointed at two different SHINOGI_HOMEs run side by side without
interfering.


A window under WSL
------------------

Headless needs nothing. For a window, WSLg supplies the display, and
there the default backend's pointer grab does not release: the pointer
enters the window and then will not leave. Ask for GTK instead:

    ./shinogi run --display gtk

That is a display-backend quirk, not an emulator fault, and it affects
remote-desktop sessions on ordinary Linux the same way.


Why the bundle carries a C library
----------------------------------

The emulator is built on a current distribution and references symbol
versions that older ones do not have; on a host without them it refuses
to start with a message about GLIBC that reads like a corrupt download.
So the loader and the C library it was built against travel with it, in
lib/glibc rather than in lib with everything else. That separation
matters: the launcher tries the host's own loader first, and if the
bundle's C library were on the search path for that attempt, the host's
loader would pair itself with our newer libc and crash outright instead
of failing politely. Kept apart, the first attempt uses the host's C
library and the fallback names ours explicitly. "shinogi doctor" reports
which of the two is in use.


Version
-------

BUILD.txt records the exact edition, CPU, emulator version and the
checksum of the ROM image in this bundle.

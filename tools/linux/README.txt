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
    ./shinogi log -f                    # watch the serial console
    ./shinogi screenshot desktop.png    # capture the screen
    ./shinogi key ret                   # press Return
    ./shinogi type "hello"              # send text
    ./shinogi mouse move 640 400
    ./shinogi mouse click
    ./shinogi stop

"shinogi screenshot" writes a real PNG of the guest's screen whether or
not there is a display attached, so a boot can be verified from a
terminal. "shinogi log" is the guest's serial console, which carries the
kernel's own messages and is usually where a failed boot explains itself.

"shinogi monitor <command>" passes a command straight to the emulator's
monitor if you need something the launcher does not wrap - "info block",
"info network", "stop", "cont", and so on.


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
    ./bin/                   the emulator and its two helpers
    ./lib/                   its libraries, including the C library
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
So the loader and the C library it was built against travel with it. The
launcher tries the host's own loader first and only falls back to the
bundled one, so a machine new enough to run the binary directly does.
"shinogi doctor" reports which of the two is in use.


Version
-------

BUILD.txt records the exact edition, CPU, emulator version and the
checksum of the ROM image in this bundle.

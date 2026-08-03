# Shinogi

**A modern Atari GEM desktop, on the machine you already have.**

---

*Shinogi* (凌ぎ) is a term from the game of Go. It means surviving with a weak
group deep inside enemy territory — a position with no room, surrounded on every
side, kept alive by skill alone.

That is the Atari 68k community, and has been for forty years.

---

Shinogi runs EmuTOS, FreeMiNT and XaAES on QEMU's `m68k virt` machine — with
truecolor at modern resolutions, working networking, and your host filesystem
as the system drive. There is no disk image — the files GEM sees are ordinary
files in a folder you can open, edit, back up and version control from the host.
Download one thing, run it, get a desktop.

## Why this exists

Everything Shinogi does is already possible. ARAnyM, EmuTOS, FreeMiNT, fVDI and
XaAES are mature software, and between them they already cover modern
resolutions, host filesystem mapping, USB and networking. The pieces are good.

What's missing is a version where they arrive already agreeing with each other.
Shinogi is that version: a modern FreeMiNT desktop with XaAES, truecolor at a
resolution you'd actually use, networking with nothing to configure on the host,
and your own files visible as a drive. Working on first launch — no setup step,
no config files to edit.

## What it is not

**Not a new emulator.** QEMU is the emulator, and it is not modified here. What
this project contributes is an EmuTOS machine port, FreeMiNT drivers for virtio
devices, and packaging. Choosing not to write an emulator is the point — it's
what makes a maintained JIT, working networking, snapshots and every host
platform arrive for free.

**Not cycle-accurate.** Shinogi targets a virtual machine, not a real Atari.
Timing does not match a Falcon or a TT, and it never will. For accuracy work —
demos, timing-sensitive software, anything where "does this run on real
hardware" is the question — use Hatari. Both tools are correct for their job.

**Not a substitute for hardware testing.** Emulation is more forgiving than
silicon. Cache coherency bugs, timing races, and FPU precision differences are
invisible here and real on a machine. Software that works in Shinogi has not
been proven to work on an Atari.

## Status

Early. Nothing here is usable yet.

| Phase | Milestone | State |
|-------|-----------|-------|
| 0 | QEMU virt machine map documented from source | done |
| 1 | EmuTOS boots far enough to print to console | done |
| 2 | Timer and interrupts | done |
| 3 | virtio-mmio + virtqueue core | done |
| 4 | Display via virtio-gpu | done |
| 5 | Host filesystem as system drive, via virtio-9p | in progress |
| 6 | FreeMiNT + XaAES desktop | |
| 7 | Networking via virtio-net | |
| 8 | Optional virtio-blk for mounting existing Atari images | |
| 9 | Packaged for Linux, macOS, Windows | |

## Design

Target CPU is the **68040**. Deliberate: the 68060 removed integer instructions
the 040 has in hardware — `MOVEP`, `CAS2`, `CMP2`/`CHK2`, and the 64-bit forms
of `MULU.L`/`MULS.L`/`DIVU.L`/`DIVS.L` — all of which trap to a software package
and change behaviour for existing binaries. QEMU's 68040 path is also the
well-exercised one, since Linux/m68k and the q800 machine depend on it.

Everything the guest talks to is virtio, so host integration is QEMU's problem
rather than ours:

| Function | Device | Host side |
|----------|--------|-----------|
| Display | virtio-gpu | QEMU window, any resolution |
| System drive | virtio-9p | a folder on your machine |
| Network | virtio-net | QEMU user-mode networking, no host setup |
| Input | virtio-keyboard / virtio-tablet | absolute pointer, no grab |
| Console | goldfish-tty | stdout |
| Legacy images | virtio-blk | optional, for existing `.st` / `.img` files |

**One trap for contributors:** modern virtio structures are little-endian by
specification. m68k is big-endian. Every descriptor field, ring index and
config-space read needs an explicit byte swap. Native-endian access produces
plausible-looking garbage rather than a clean failure — route everything through
the swap helpers.

## What lives where

- **This repo** — the EmuTOS machine port, FreeMiNT virtio drivers, build
  scripts, packaging.
- **Upstream, unmodified** — QEMU, EmuTOS, FreeMiNT, XaAES.

EmuTOS changes are machine-conditional under `MACHINE_QEMU_VIRT`, following the
existing Amiga, Lisa and ColdFire ports. FreeMiNT changes are loadable drivers
where the driver model allows. Nothing is a fork; everything should be
upstreamable.

## Building

Not yet. This section lands with Phase 9.

## License

`GPL-2.0-or-later`, inherited from EmuTOS and FreeMiNT.

---

Shinogi is unaffiliated with Atari SA, Atari Interactive, and any other holder
of the Atari trademarks.

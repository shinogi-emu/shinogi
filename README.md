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

**Not a new emulator.** QEMU is the emulator. What this project contributes is
an EmuTOS machine port, FreeMiNT drivers for virtio devices, and packaging.
Choosing not to write an emulator is the point — it's what makes a maintained
JIT, working networking, snapshots and every host platform arrive for free.
There is a small patch set in `patches/`, carried against upstream QEMU and
fVDI rather than forked from them.

**Not cycle-accurate.** Shinogi targets a virtual machine, not a real Atari.
Timing does not match a Falcon or a TT, and it never will. For accuracy work —
demos, timing-sensitive software, anything where "does this run on real
hardware" is the question — use Hatari. Both tools are correct for their job.

**Not a substitute for hardware testing.** Emulation is more forgiving than
silicon. Cache coherency bugs, timing races, and FPU precision differences are
invisible here and real on a machine. Software that works in Shinogi has not
been proven to work on an Atari.

## Status

Beta. It boots to a desktop, and there are builds you can download and run —
but expect to find things that are broken.

| Phase | Milestone | State |
|-------|-----------|-------|
| 0 | QEMU virt machine map documented from source | done |
| 1 | EmuTOS boots far enough to print to console | done |
| 2 | Timer and interrupts | done |
| 3 | virtio-mmio + virtqueue core | done |
| 4 | Display via virtio-gpu | done |
| 5 | Host filesystem as system drive | done |
| 6 | FreeMiNT + XaAES desktop | done |
| 7 | Networking via virtio-net | done |
| 8 | Optional virtio-blk for mounting existing Atari images | |
| 9 | Packaged for Linux, macOS, Windows | Linux and Windows done, macOS in progress |

## Editions

Two builds, differing only in the emulated CPU. Both ship the same guest,
the same drive C and the same desktop.

| Edition | CPU | For |
|---------|-----|-----|
| **Shinogi-060** | 68060 | the default — start here |
| Shinogi-040 | 68040 | a control, and a fallback if something misbehaves on the 060 |

If you don't have a reason to pick, take **Shinogi-060**.

## Design

The primary target is the **68060**, and it is what the default edition ships.
The 68040 edition is built from the same tree and kept as a regression control:
when something behaves oddly on the 060, running the identical guest on the 040
says immediately whether the CPU model is involved.

The 060 is not a free upgrade, and the difference is worth knowing about. It
dropped integer instructions the 040 has in hardware — `MOVEP`, `CAS2`,
`CMP2`/`CHK2`, and the 64-bit forms of `MULU.L`/`MULS.L`/`DIVU.L`/`DIVS.L` —
which on real silicon trap to a software package. The bundle ships Motorola's
060 package (`060sp.prg`) to catch them, so old binaries do run — but they run
through an exception handler rather than in hardware, and anything sensitive to
that will notice. The 040 edition is there for when something does.

Everything the guest talks to is virtio, so host integration is QEMU's problem
rather than ours:

| Function | Device | Host side |
|----------|--------|-----------|
| Display | virtio-gpu | QEMU window, any resolution |
| System drive | hostfs link over virtio-serial | a folder on your machine |
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
where the driver model allows. Nothing here is a permanent fork — the patches in
`patches/` are kept as patches so they stay upstreamable.

## Building

The packaging scripts under `tools/` build a bundle for each host from a
checkout plus the m68k cross toolchain:

| Host | Script | Produces |
|------|--------|----------|
| Linux | `tools/make-linux-package.sh` | a self-contained directory + launcher |
| Windows | `tools/make-windows-package.sh` | an NSIS installer |
| macOS | `tools/make-macos-package.sh` | a signed `.app` bundle |

The Linux and Windows scripts pick their edition from `SHINOGI_CPU`
(`m68040` or `m68060`), so both builds come out of one tree. The guest
itself — EmuTOS with the machine port — is a submodule; the FreeMiNT side is
built by `tools/make-mint-install.sh`.

The macOS bundle is the unfinished one. It was written without a Mac to test
on, so its first honest run is the one in CI.

## License

`GPL-2.0-or-later`, inherited from EmuTOS and FreeMiNT.

---

Shinogi is unaffiliated with Atari SA, Atari Interactive, and any other holder
of the Atari trademarks.

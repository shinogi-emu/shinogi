# Amendment — the host folder is the system drive

**Supersedes Phases 5 and 8 of the original plan. Phases 0-4 are unchanged
and nothing already delivered is invalidated.**

There is no disk image. The guest's system drive is a folder on the host,
exposed over virtio-9p. Everything GEM sees is an ordinary host file.

virtio-blk survives only as an optional convenience for mounting existing
Atari `.st` / `.msa` / `.img` files, and moves to the end of the plan.

## Why

- Deletes "how do I get files in and out" as a question entirely.
- Makes the guest filesystem version-controllable and backup-able as a
  normal directory.
- Removes image build, mount and resize from the packaging story.

## Revised phase order

| # | Phase | Was |
|---|---|---|
| 5 | virtio-9p — host folder as the system drive | virtio-blk |
| 6 | FreeMiNT + XaAES, loaded from that folder | (now depends on 5) |
| 7 | virtio-net | unchanged |
| 8 | virtio-blk — optional, legacy image mounting | virtio-9p |
| 9 | Packaging | unchanged |

Tracked as `shin-apn.9` (Phase 5), `shin-apn.7` (Phase 6), `shin-apn.8`
(Phase 7), `shin-apn.6` (Phase 8). The bead IDs keep their original
numbering; only the titles and dependency edges changed.

## The 8.3 constraint

**EmuTOS's GEMDOS is 8.3 only.** Long filenames on Atari are a MiNT
extension, not a TOS one: `Dopendir`/`Dreaddir` expose long names,
`Fsfirst`/`Fsnext` do not.

At Phase 5 there is no MiNT layer yet — EmuTOS is reading the host folder
in order to find and load FreeMiNT. **The boot path is therefore 8.3, and
only becomes long-name-capable after Phase 6.**

After Phase 6 the folder has two views over one directory:

| Interface | Sees |
|---|---|
| `Fsfirst` / `Fsnext` | 8.3 clipped names |
| `Dopendir` / `Dreaddir` | real long host names |

## Filename mapping rules

**Follow Hatari's GEMDOS drive behaviour exactly.** The goal is that the
same host folder behaves identically under Hatari and under Shinogi. Where
this document and Hatari's actual behaviour disagree, Hatari wins — the
same way QEMU wins over the virtio spec.

1. **Clip host names to 8+3 the way TOS does.** Do not invent a scheme.
2. **Enumerate directories in sorted order, always.** This is not
   cosmetic: AUTO folder execution order depends on it, and host
   filesystems provide no inherent ordering.
3. **On collision, first in sort order wins.** When several host files
   clip to the same 8+3 name, the first one in sort order is the one GEM
   sees. Document it in the user-facing docs. **Do not add `~1` / `~2`
   suffixes.**
4. **Case.** GEMDOS is case-insensitive and uppercase; the host may be
   case-sensitive. Names differing only in case collide under rule 3.
5. **Attributes.** Map the GEMDOS attribute bits — read-only, hidden,
   system, archive — onto host permissions approximately, and document
   the mapping. **No sidecar metadata files.**

Rule 3 is the one users will actually hit, and rule 2 is the one that
silently breaks boots if it is missed.

## Shipping rule

Everything EmuTOS itself must find — `MINT.PRG`, the `AUTO` folder,
drivers, config — **has to be 8.3-clean in the bundled tree**, because
the boot path runs through EmuTOS's 8.3-only GEMDOS before any MiNT layer
exists.

User content is unconstrained.

## Restore defaults

A host folder the user can edit is a host folder the user can break, and
unlike a disk image there is no pristine copy to fall back to. **The
bundled default tree must be re-extractable on demand.**

Tracked as `shin-apn.14`. It can land any time after Phase 5, and is
wanted before Phase 9 packaging ships.

## Phase 5 gate

EmuTOS boots, mounts a host folder as its system drive, and loads and runs
a program from it. Then verify all four:

1. A file created from GEM appears on the host with the expected name.
2. A long-named host file is visible from GEM with the correct clipped
   name.
3. Two colliding names resolve identically across repeated boots.
4. AUTO folder order matches sort order.

## Phase 8 gate

A community disk image mounts as a second drive, readable from GEM. Not
the boot device, not required for a working system, shipped disabled by
default.

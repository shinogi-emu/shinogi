# Phase 5 design — the host folder as the GEMDOS system drive

Design for `shin-apn.9`. Read `plan-amendment-host-folder.md` for *why* the
system drive is a host folder, and `hatari-gemdos-reference.md` for the
behaviour being reproduced. This document is *how*.

## Decisions taken

Two questions were open before this design. Both are now settled.

**Collisions: match Hatari exactly.** When several host files clip to the
same 8.3 name, every one is listed — with identical `dta_name` — and
lookup takes the first `strcasecmp` hit in raw `readdir()` order. There is
no deduplication and no `~1` suffixing.

This is nondeterministic: which file `Fopen` returns is not stable across
boots or filesystems. That is a deliberate trade, taken to keep byte-parity
with Hatari, and it means **gate criterion 3 is dropped** — see "Gate"
below. The hazard is real where the `AUTO` folder is concerned, and the
mitigation is the shipping rule from the amendment: everything EmuTOS must
find has to be 8.3-clean in the bundled tree, so it cannot collide.

**Delivery is staged.** Stage 1 is read-only and ends with a bootable,
browsable host folder. Stage 2 adds writes and completes the gate.

## Feasibility, already confirmed

`virtio-9p-device` exists on this QEMU's `virtio-bus`, and the guest's
existing probe already finds it without any new code:

```
virtio: slot 124 at 0xff01f800: id 9 (9p) v2 vendor 0x554d4551
virtio: slot 124 offered 00000101:30000001
virtio: 4 device(s)
```

Feature bit 0 is `VIRTIO_9P_MOUNT_TAG`, so the mount tag is readable from
config space. Host side:

```sh
-fsdev local,id=hostfs,path=<dir>,security_model=mapped-xattr
-device virtio-9p-device,fsdev=hostfs,mount_tag=shinogi
```

## Architecture

Three units. Each has one job, and the dependency edges run one way.

| Unit | Owns | Depends on |
|---|---|---|
| `bios/virtio_9p.c` | the virtqueue, 9P2000.L encode/decode, errno mapping | `virtq_request()` |
| `bdos/hostfs_name.c` | 8.3 clipping, sort order — pure logic, no I/O | nothing |
| `bdos/hostfs.c` | fids, handles, DTA state, path resolution, the hook | both |

`virtio_9p.c` knows nothing about GEMDOS; `hostfs_name.c` knows nothing
about 9P. Only `hostfs.c` knows both. The split exists so the fiddly
part — the name rules — can be tested on the host without booting.

**9P2000.L, not 9P2000.u.** `Treaddir` returns names directly and
`Tgetattr` returns a fixed-layout struct, where 9P2000.u needs
variable-length `stat` parsing for every operation. Less guest code for
the same result.

Transport API, all synchronous on top of the Phase 3 helper:

```
p9_attach()   p9_walk()     p9_lopen()    p9_read()
p9_readdir()  p9_getattr()  p9_clunk()
```

Stage 2 adds `p9_lcreate()`, `p9_write()`, `p9_unlinkat()`, `p9_rename()`,
`p9_mkdir()`, `p9_setattr()`.

## Data flow

`Fopen("C:\AUTO\FOO.PRG")`:

```
osif() hook
  -> hostfs claims drive C
  -> split path into components
  -> per component: p9_readdir(parent)
       -> strcasecmp over RAW host names
       -> first hit in readdir order wins   (no dedup, per decision)
  -> p9_walk to the real host name
  -> p9_lopen
  -> allocate GEMDOS handle
```

## Drive registration

At boot, if the probe found a device with id 9, set `drvbits |= 1 << 2`
for `C:` and point `bootdev` at it.

If no 9p device is present nothing registers, and EmuTOS behaves exactly
as it does today. The feature is absent rather than broken — which also
keeps every existing boot path working when the launcher omits the fsdev.

## The hook

One machine-conditional insertion in `osif()` (`bdos/bdosmain.c:431`),
placed after the `fn > MAX_FNCALL` bounds check:

```c
#ifdef MACHINE_QEMU_VIRT
    if (hostfs_claims(fn, pw))
        return hostfs_dispatch(fn, pw);
#endif
```

`hostfs_claims()` is a switch over the function number that decides
whether this particular call refers to our drive. How the drive is
identified varies by call and there is no way around knowing it per call:

| Identified by | Calls |
|---|---|
| a path argument | `Fopen` `Fsfirst` `Fcreate` `Fdelete` `Frename` `Dcreate` `Ddelete` `Dsetpath` `Fattrib` |
| an open handle | `Fread` `Fwrite` `Fclose` `Fseek` `Fdatime` |
| a drive number | `Dsetdrv` `Dfree` |
| current drive | `Dgetdrv` `Dgetpath` `Fsnext` |

Anything not claimed falls through to stock EmuTOS untouched. The diff to
existing EmuTOS files is this hook and the `drvbits` registration; all
other code is new files, so the change stays upstreamable as a
machine-conditional feature in the Amiga/Lisa/ColdFire style.

### Call surface by stage

| Stage 1 (read) | Stage 2 (write) |
|---|---|
| `Dsetdrv` `Dgetdrv` `Dsetpath` `Dgetpath` | `Fcreate` `Fwrite` `Fdelete` `Frename` |
| `Fsfirst` `Fsnext` `Fopen` `Fclose` `Fread` `Fseek` | `Dcreate` `Ddelete` `Fdatime` |
| `Fattrib` (query) `Dfree` | `Fattrib` (set) |

## Name mapping

The two conversions are **not inverses**. This is the single biggest trap
and the source of most of the surprising behaviour.

| Direction | Rule | Example |
|---|---|---|
| host → Atari (what `Fsfirst` shows) | split on **last** dot; earlier dots → `+`; base→8, ext→3; upcase; `<32`, `127`, `* : ? \ /` → `+` | `a.b.c` → `A+B.C`, `foo.html` → `FOO.HTM` |
| Atari → host (what `Fopen` looks up) | split on **first** dot; no substitution, no upcase | — |

`INVALID_CHAR` is `'+'`. There is no truncation marker, ever.

Three rules a naive implementation gets wrong:

1. **Wildcards match raw host names, not clipped names.** `readme.txt.bak`
   does not match `*.*` even though it displays as `README+T.BAK`;
   `????????.TXT` does not match `verylongname.txt`. Listing membership
   and display names are computed independently.
2. **The sort is case-SENSITIVE byte order** — `strcmp` in the C locale,
   so all `A-Z` precede all `a-z`. Hatari's manual claims otherwise and is
   wrong. Sorted once at `Fsfirst`; `Fsnext` walks the array by index, so
   order is fixed for the life of the DTA.
3. **Dot entries.** At the drive root every `.`-prefixed entry is dropped,
   including `.` and `..`. In subdirectories `.` and `..` are kept and
   other dot-files are dropped.

Attributes carry only two bits: `SUBDIRECTORY` iff a directory, `READONLY`
iff not writable. Hidden, system and archive are never set, and writes to
them are silently ignored while the call still returns the requested
value. The attribute filter ORs in `0x21`, so read-only directories are
returned even when `0x10` was not requested.

`stat()` not `lstat()`: symlinks are followed and dangling ones are
skipped from listings. Dates use `localtime()` on `st_mtime` at 2-second
resolution; pre-1980 timestamps clamp the year field only, leaving month
and day alone. The volume label is the synthetic `EMULATED.00N`.

## Error handling

One errno → GEMDOS mapping table, in `virtio_9p.c`, so `hostfs.c` only
ever sees GEMDOS codes:

| 9P `Rlerror` | GEMDOS |
|---|---|
| `ENOENT` | `EFILNF` (-33) |
| `ENOTDIR` | `EPTHNF` (-34) |
| `EACCES`, `EPERM`, `EROFS` | `EACCDN` (-36) |
| `EMFILE`, `ENFILE` | `ENHNDL` (-35) |
| `ENOSPC` | `ENSMEM` (-39) |
| anything else | `EACCDN` (-36) |

Transport failure — no reply within the bounded spin already used by the
GPU and input drivers — returns `EDRIVE` (-46). **A dead device must fail
the call, not hang the machine.** That bound is the one raised to 20M in
Phase 4; it is not a timeout in any real sense, just a guarantee of
termination.

`..` is resolved textually and refuses to climb above the drive root, and
`Dsetpath` re-verifies by prefix comparison. Path escape is a
correctness requirement, not a nicety: the host folder is the user's real
filesystem.

## Testing

**Name mapping is pure, so it is tested on the host.** `hostfs_name.c`
compiles for the build machine and gets a table-driven test in `tests/`
covering every case in `hatari-gemdos-reference.md`. Written before the
implementation — the rules are already specified, so there is nothing to
discover by writing the code first.

**Differential test against Hatari.** Hatari's source is on this machine.
Pointing both emulators at the same host folder and diffing the directory
listings is the only way to catch a rule transcribed wrongly, and parity
with Hatari is now an explicit requirement rather than an aspiration.

**Boot behaviour** gets a golden serial log, in the style of
`tests/golden/boot-phase2.log`.

## Gate

From the amendment, as amended by the collision decision:

1. A file created from GEM appears on the host with the expected name.
2. A long-named host file is visible from GEM with the correct clipped
   name.
3. ~~Two colliding names resolve identically across repeated boots.~~
   **Dropped.** Incompatible with exact Hatari parity, which was chosen
   instead. Recorded here so it does not read as an unmet criterion.
4. `AUTO` folder execution order matches sort order — byte order, not
   case-insensitive.

Stage 1 is complete when EmuTOS boots from the host folder and a
directory listing in GEM matches the host. Stage 2 completes 1, 2 and 4.

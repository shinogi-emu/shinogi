# Hatari GEMDOS drive behaviour — implementation reference

Phase 5 exposes a host folder as the GEMDOS system drive, and the design
rule is that the same folder must behave identically under Hatari and
under Shinogi. This records what Hatari actually does, from source.

Source: `/home/rob/git/Hatari` at `275dae74698a` (2026-07-27). Primary
files `src/gemdos.c`, `src/str.c`, `src/scandir.c`.

> Where this document and Hatari's source disagree, the source wins.
> Where Hatari's *manual* and its source disagree, the source also wins —
> there is at least one such case, noted below.

## The single biggest trap: two asymmetric conversions

Host→Atari and Atari→host are **different code paths with different
rules**. They are not inverses.

| Direction | Function | Dot rule |
|---|---|---|
| host → Atari (what `Fsfirst` *shows*) | `Str_Filename_Host2Atari()` `str.c:325-386` | `strrchr` — **last** dot |
| Atari → host (what `Fopen` *looks up*) | `clip_to_83()` `gemdos.c:1239-1268` | `strchr` — **first** dot |

The display path also substitutes invalid characters and upcases; the
lookup path does neither.

## 1. Clipping a host name to 8.3

`Str_Filename_Host2Atari()`, in order (`str.c:325-386`):

1. Optional charset conversion — **off by default** (`configuration.c:660`,
   flag `--gemdos-conv`). By default raw host bytes pass through and
   bytes ≥ 0x80 are untouched.
2. Extension = text after the **last** dot (`str.c:338`).
3. Extension truncated to 3 (`str.c:341-343`): `foo.html` → `FOO.HTM`.
4. Every dot **before** the last becomes `INVALID_CHAR` (`str.c:345-351`).
   Sole exception: the literal name `..`. So `a.b.c` → `A+B.C`.
5. Base truncated to 8, extension appended directly (`str.c:353-355`):
   `longfilename.txt` → `LONGFILE.TXT`.
6. No dot at all and longer than 8 → hard truncate to 8 (`str.c:357-358`).
7. Bytes < 32, byte 127, and `* : ? \ /` → `INVALID_CHAR`; everything
   else < 128 → `toupper` (`str.c:364-385`).

`INVALID_CHAR` is `'+'` (`str.h:35`). **There is no truncation marker —
no `~1`, ever.**

Leading-dot files convert oddly (`.bashrc` → `.BAS`) but are filtered out
of listings entirely, so this is only observable via direct open.

## 2. Collisions — Hatari is NOT deterministic

**Hatari does no deduplication at all.**

- `Fsfirst`/`Fsnext` **return every colliding entry, with identical
  `dta_name`**. The 8.3 conversion happens per-entry in `PopulateDTA()`
  (`gemdos.c:350`), after matching, with no cross-entry state. The manual
  admits it: *"they show as duplicates on the Atari side (and correct
  mapping cannot be guaranteed)"* (`doc/manual.html:2770-2772`).
- **Lookup** takes the first `strcasecmp` hit in raw `readdir()` order
  (`match_host_dir_entry()`, `gemdos.c:1174-1227`, `break` at `:1216`).
  That is filesystem order — not sorted, not stable across filesystems.

See "Open decision" below: this cannot be reconciled with our gate.

## 3. Directory ordering

Sorted **once**, at `Fsfirst` time:

```c
count = scandir(path, &files, 0, alphasort);   /* gemdos.c:3161 */
```

- Sort key is the **raw host name**, before any clipping or upcasing.
- Filtering and 8.3 conversion happen afterwards and never reorder
  (`gemdos.c:3175-3193`).
- `Fsnext` walks the array by index (`gemdos.c:3015-3033`), so order is
  fixed for the life of the DTA.

**The sort is case-SENSITIVE byte order in practice.** `alphasort` is
`strcoll`, but Hatari only calls `setlocale()` under `WIN32` or
`USE_LOCALE_CHARSET`, and the latter is commented out by default
(`str.h:25`). So the locale stays `"C"` and `strcoll` degenerates to
`strcmp` — all `A-Z` before all `a-z`.

`doc/manual.html:2763-2766` claims the sort is case-*insensitive*. **The
manual is wrong**; Hatari's own source comments acknowledge it
(`/* alphasort isn't case insensitive */`, `gemdos.c:704`).

## 4. Case

- Lookup: case-insensitive by brute-force `strcasecmp` over every
  directory entry (`gemdos.c:1215`). No case-folded index, no Unicode
  folding.
- Presentation: always upper case for bytes < 128 (`str.c:381-382`).
- Creating host files: configurable, defaults to verbatim
  (`GEMDOS_NOP` → `to_same()`, `gemdos.c:1230-1233`). Since TOS hands
  names down already upper-cased, **new host files land UPPER CASE**.

## 5. Attributes

`GemDOS_ConvertAttribute()` (`gemdos.c:286-304`) sets **only two bits**:

| Bit | Behaviour |
|---|---|
| `SUBDIRECTORY` 0x10 | set iff `S_ISDIR` |
| `READONLY` 0x01 | set iff `!(mode & S_IWUSR) \|\| access(path, W_OK) != 0` |
| `HIDDEN` 0x02 | **never set** (TODO at `gemdos.c:299`) |
| `SYSTEM` 0x04 | **never set** |
| archive 0x20 | **never set** |

Writing attributes honours only read-only; hidden/system/archive requests
are silently ignored (`gemdos.c:2745-2749`) yet the call still returns the
*requested* value (`gemdos.c:2741`).

Two consequences worth reproducing exactly:

- `stat()` not `lstat()` — **symlinks are followed**; dangling symlinks
  are silently skipped from listings.
- The attribute filter ORs in `IGNORED_FILE_ATTRIBS` (`0x21`), so
  **read-only directories are always returned even when `0x10` was not
  requested** (`gemdos.c:338-342`). A quirk, but observable.

Volume label is synthetic: the literal `EMULATED.00N` where N is the
drive number (`gemdos.c:3133-3144`).

## 6. Things a reimplementation gets wrong

**Wildcards match HOST names, not clipped names.** `fsfirst_match()`
compares the Atari pattern against the raw `d_name` (`gemdos.c:3183`),
before clipping. So:

- `readme.txt.bak` does **not** match `*.*` — the extra dot breaks the
  walk — even though it displays as `README+T.BAK`.
- `????????.TXT` does **not** match `verylongname.txt` even though it
  displays as `VERYLONG.TXT`.
- A pattern of exactly `"*"` matches nothing containing a dot
  (`gemdos.c:448-449`).

Listing membership and display names are computed independently.

**Dot entries.** At the drive root, every entry starting with `.` is
dropped including `.` and `..`; in subdirectories `.` and `..` are kept
but other dot-files are dropped (`fsfirst_match`, `gemdos.c:433-449`).

**Reverse-lookup heuristics** (`add_path_component()`, `gemdos.c:1279-1415`)
run in order: exact case-insensitive match; a TOS 1.02 file-selector
workaround for 9-char names ending in `.`; truncation-recovery patterns
inserting `*` at offset 8 and after a maxed extension; and
invalid-char recovery turning each `+` back into a `?` that matches only
characters that could not have survived conversion. Failure to match is
**normal**, not an error — it is the path `Fcreate` takes.

**Reserved device names** are not special-cased. Any 4-char name ending
in `:` resolves to drive 0, which is never GEMDOS-emulated, so it falls
through to real TOS (`gemdos.c:1111-1128`).

**Path escape** is blocked: `..` is handled textually and refuses to
strip below the drive root (`gemdos.c:1527-1542`), with `Dsetpath`
re-verifying by prefix comparison (`gemdos.c:1972-1984`).

**Dates** use `localtime()` on `st_mtime`, 2-second resolution. Pre-1980
timestamps clamp the year field to 0 **but leave month and day alone**
(`gemdos.c:211-212`) — they do not become 1980-01-01.

**Single vs multi-partition**: if every non-dot entry in the root is a
single character `C`–`Z`, each becomes a drive; otherwise the whole
directory is `C:` (`gemdos.c:654-722`). An empty directory means
single-partition.

## Open decision — determinism vs Hatari parity

The amendment specifies "first in sort order wins" for collisions and
lists as a gate criterion that *"two colliding names resolve identically
across repeated boots."*

**Hatari cannot satisfy that.** It lists duplicates and resolves lookups
in `readdir()` order, which is not stable. So exact Hatari parity and the
determinism gate are mutually exclusive, and one of them has to give:

1. **Match Hatari exactly** — list duplicates, accept nondeterministic
   lookup. Drop gate criterion 3.
2. **Diverge deliberately** — dedup by sort order so collisions resolve
   identically every boot. Keeps the gate, and is better behaviour, but
   the same folder then enumerates differently under Hatari and Shinogi.

Option 2 is the recommendation: nondeterministic file resolution in the
boot path is a real hazard when the AUTO folder is involved, and the
divergence only shows up in a folder that already contains colliding
names. It must be documented as an intentional deviation rather than
described as parity.

Note the sort question is separate and has no such tension: byte-order
`strcmp` on raw host names reproduces default Hatari exactly, and should
be adopted as-is. Our amendment did not specify the collation, and
"sorted" would naturally have been read as case-insensitive — which would
have been wrong.

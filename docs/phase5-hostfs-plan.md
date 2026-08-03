# Phase 5 Stage 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** EmuTOS boots with a host folder as drive `C:` over virtio-9p, and a directory listing in GEM matches the host folder.

**Architecture:** Three new units with one-way dependencies — a 9P2000.L transport over the existing Phase 3 virtqueue helper, a pure 8.3 name-mapping unit with no I/O, and a GEMDOS layer that hooks `osif()` and owns fids, handles and DTA state. Stage 1 is read-only; writes are Stage 2.

**Tech Stack:** C (m68k, `-mshort`), EmuTOS BIOS/BDOS, virtio-mmio, 9P2000.L, QEMU `virtio-9p-device`.

## Global Constraints

- **Two repos.** Guest code lives in `/home/rob/git/emutos` on branch `shinogi`. Tests and docs live in `/home/rob/git/shinogi`. Commit to both; push both.
- **`-mshort`: `int` is 16 bits.** Any integer constant expression near 32768 overflows. Use `LONG`/`ULONG` for all 9P 32-bit fields and never rely on `int` width. This has already caused one shipped bug.
- **9P fields are little-endian; m68k is big-endian.** Every multi-byte field needs explicit byte assembly. Do not cast structs over the buffer.
- **All EmuTOS changes stay under `#ifdef MACHINE_QEMU_VIRT`**, following the Amiga/Lisa/ColdFire pattern, and must be upstreamable.
- **No AI/agent authorship anywhere** — code, comments, commit messages, trailers, docs.
- **Never send anything to a mailing list**, and do not propose it.
- **Do not modify QEMU.**
- Build: `make ELF=1 TOOLCHAIN_PREFIX=m68k-atari-mintelf- qemu-virt`, and `make clean` first when switching targets.
- Behaviour reference: `docs/hatari-gemdos-reference.md`. Where it and Hatari's source disagree, **the source wins**.
- Collisions: **no deduplication.** Duplicates are listed; lookup takes the first `strcasecmp` hit in raw `readdir()` order.

---

## File Structure

| File | Responsibility |
|---|---|
| `emutos/bdos/hostfs_name.c` `.h` | 8.3 clipping both directions, sort comparison. Pure; no I/O, no 9P, no GEMDOS. |
| `emutos/bios/virtio_9p.c` `.h` | 9P2000.L encode/decode over one virtqueue; errno→GEMDOS mapping. Knows nothing of GEMDOS paths. |
| `emutos/bdos/hostfs.c` `.h` | Drive registration, path resolution, fid/handle/DTA state, the `osif()` hook. |
| `emutos/bios/virtio.c:484` | Add the id-9 dispatch line. |
| `emutos/bdos/bdosmain.c:444` | Add the hook after the bounds check. |
| `emutos/Makefile:311,324` | Add new sources. |
| `shinogi/tests/hostfs-name/` | Native host test for the name unit. |
| `shinogi/tools/hostfs-difftest.sh` | Differential listing test against Hatari. |

---

### Task 0: golden-log test runner

**Files:**
- Create: `shinogi/tools/run-golden.sh`

**Interfaces:**
- Consumes: nothing.
- Produces: `tools/run-golden.sh <name> <grep-ere>` — boots the guest
  against a host folder, extracts matching serial lines, diffs them
  against `tests/golden/<name>.expected`, and exits non-zero on any
  mismatch. Every later task's verification step calls this.

Without this the `.expected` files are documentation, not tests: nothing
compares them to anything and a regression would be invisible.

- [ ] **Step 1: Write the runner**

Create `shinogi/tools/run-golden.sh`:

```sh
#!/bin/sh
#
# Boot the guest and check its serial output against a golden file.
#
# Usage: tools/run-golden.sh <name> <grep-ere> [host-folder]
#
#   <name>      tests/golden/<name>.expected holds the expected lines
#   <grep-ere>  extended regex selecting the lines to compare
#
# Exits 0 only when the extracted lines match the golden file exactly,
# in order. Any mismatch prints a diff and exits 1.
#
set -eu

NAME="${1:?usage: run-golden.sh <name> <grep-ere> [host-folder]}"
PATTERN="${2:?usage: run-golden.sh <name> <grep-ere> [host-folder]}"
FOLDER="${3:-/tmp/shinogi-hostfs}"

ROOT=$(cd "$(dirname "$0")/.." && pwd)
ELF="${SHINOGI_ELF:-$HOME/git/emutos/emutos-virt.elf}"
GOLDEN="$ROOT/tests/golden/$NAME.expected"
WORK="${TMPDIR:-/tmp}/run-golden-$NAME"
BOOT_WAIT="${BOOT_WAIT:-25}"

[ -f "$ELF" ]    || { echo "no guest image at $ELF" >&2; exit 2; }
[ -f "$GOLDEN" ] || { echo "no golden file at $GOLDEN" >&2; exit 2; }

mkdir -p "$WORK" "$FOLDER"
LOG="$WORK/serial.log"
rm -f "$LOG"

qemu-system-m68k \
    -M virt -m 128 \
    -kernel "$ELF" \
    -device virtio-gpu-device \
    -fsdev "local,id=hostfs,path=$FOLDER,security_model=mapped-xattr" \
    -device virtio-9p-device,fsdev=hostfs,mount_tag=shinogi \
    -display none \
    -serial "file:$LOG" \
    -d guest_errors -D "$WORK/guest-errors.log" &
QPID=$!

# The guest never exits on its own; give it a fixed window then stop it.
i=0
while [ "$i" -lt "$BOOT_WAIT" ]; do
    sleep 1
    i=$((i + 1))
done
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true

# The guest emits CRLF; strip the CR so goldens can be plain LF.
grep -aoE "$PATTERN" "$LOG" | tr -d '\r' > "$WORK/actual" || true

if diff -u "$GOLDEN" "$WORK/actual"; then
    echo "PASS $NAME"
    exit 0
fi

echo "FAIL $NAME - see $LOG" >&2
exit 1
```

```bash
chmod +x ~/git/shinogi/tools/run-golden.sh
```

- [ ] **Step 2: Verify it fails honestly when nothing matches**

```bash
cd ~/git/shinogi
mkdir -p tests/golden
printf 'this line will never appear\n' > tests/golden/selftest.expected
tools/run-golden.sh selftest 'this line will never appear'; echo "exit=$?"
```
Expected: a diff showing the golden line missing, `FAIL selftest`, `exit=1`.
**A runner that passes here is broken** — it must not report success
without having matched anything.

- [ ] **Step 3: Verify it passes on a line the guest really prints**

```bash
cd ~/git/shinogi
printf 'virtio: 2 device(s)\n' > tests/golden/selftest.expected
tools/run-golden.sh selftest 'virtio: [0-9]+ device\(s\)'; echo "exit=$?"
```
Expected: `PASS selftest`, `exit=0`. The count is 2 here because the
runner passes only the GPU and the 9p device.

- [ ] **Step 4: Remove the scratch golden file and commit**

```bash
cd ~/git/shinogi && rm -f tests/golden/selftest.expected
git add tools/run-golden.sh && git commit -m "tools: check guest serial output against golden files"
```

---

### Task 1: 8.3 name mapping, host→Atari

**Files:**
- Create: `emutos/bdos/hostfs_name.c`, `emutos/bdos/hostfs_name.h`
- Create: `shinogi/tests/hostfs-name/test_hostfs_name.c`, `shinogi/tests/hostfs-name/Makefile`

**Interfaces:**
- Consumes: nothing.
- Produces: `void hostfs_host_to_atari(const char *host, char *out);` — `out` must be at least 13 bytes and receives a NUL-terminated `8.3` name.

- [ ] **Step 1: Write the failing test**

Create `shinogi/tests/hostfs-name/test_hostfs_name.c`:

```c
/* Native test for the 8.3 mapping. Cases come from
 * docs/hatari-gemdos-reference.md section 1. */
#include <stdio.h>
#include <string.h>

#include "hostfs_name.h"

static int failures;

static void check(const char *host, const char *want)
{
    char got[13];

    hostfs_host_to_atari(host, got);
    if (strcmp(got, want) != 0)
    {
        printf("FAIL host2atari(\"%s\") = \"%s\", want \"%s\"\n",
               host, got, want);
        failures++;
    }
}

int main(void)
{
    /* Extension is text after the LAST dot, truncated to 3. */
    check("foo.html", "FOO.HTM");
    check("readme.txt", "README.TXT");

    /* Dots before the last become '+'. */
    check("a.b.c", "A+B.C");
    check("readme.txt.bak", "README+T.BAK");

    /* Base truncated to 8. */
    check("longfilename.txt", "LONGFILE.TXT");

    /* No dot at all and longer than 8: hard truncate. */
    check("verylongname", "VERYLONG");

    /* Invalid characters become '+'; everything else upcases. */
    check("file*name.txt", "FILE+NAM.TXT");
    check("a?b.t", "A+B.T");

    /* Short names pass through, upcased. */
    check("hello.c", "HELLO.C");
    check("MiXeD.TxT", "MIXED.TXT");

    /* Leading-dot files convert oddly but predictably. */
    check(".bashrc", ".BAS");

    /* The literal ".." is the sole exception to the dot rule. */
    check("..", "..");

    if (failures == 0)
        printf("all host2atari cases passed\n");
    return failures != 0;
}
```

Create `shinogi/tests/hostfs-name/Makefile`:

```make
# Builds the pure name-mapping unit natively, straight out of the
# EmuTOS tree, so the Hatari rules can be checked without booting.
EMUTOS ?= $(HOME)/git/emutos

CFLAGS = -Wall -Wextra -O1 -I$(EMUTOS)/bdos -I.

test: test_hostfs_name
	./test_hostfs_name

test_hostfs_name: test_hostfs_name.c $(EMUTOS)/bdos/hostfs_name.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f test_hostfs_name

.PHONY: test clean
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd ~/git/shinogi/tests/hostfs-name && make test`
Expected: FAIL — `hostfs_name.h: No such file or directory`

- [ ] **Step 3: Write minimal implementation**

Create `emutos/bdos/hostfs_name.h`:

```c
/*
 * hostfs_name.h - 8.3 name mapping for the host-folder drive
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef HOSTFS_NAME_H
#define HOSTFS_NAME_H

/* Clip a host name to a NUL-terminated 8.3 name. out >= 13 bytes. */
void hostfs_host_to_atari(const char *host, char *out);

#endif /* HOSTFS_NAME_H */
```

Create `emutos/bdos/hostfs_name.c`:

```c
/*
 * hostfs_name.c - 8.3 name mapping for the host-folder drive
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * Pure name logic: no I/O, no 9P, no GEMDOS. Kept separate so the
 * rules can be tested natively.
 *
 * The host->Atari and Atari->host conversions are NOT inverses, and
 * that asymmetry is deliberate rather than an oversight -- it is what
 * the reference implementation does. This file is the host->Atari
 * (display) direction: extension is the text after the LAST dot.
 */

#include "hostfs_name.h"

#define INVALID_CHAR '+'

/* Bytes below 32, byte 127, and these become INVALID_CHAR. */
static int is_invalid(unsigned char c)
{
    if (c < 32 || c == 127)
        return 1;
    return c == '*' || c == ':' || c == '?' || c == '\\' || c == '/';
}

static char convert(unsigned char c)
{
    if (is_invalid(c))
        return INVALID_CHAR;
    if (c >= 128)
        return (char)c;             /* raw high bytes pass through */
    if (c >= 'a' && c <= 'z')
        return (char)(c - 'a' + 'A');
    return (char)c;
}

void hostfs_host_to_atari(const char *host, char *out)
{
    const char *dot = 0;
    const char *p;
    int base = 0, ext = 0, n = 0;

    /* ".." is the sole name exempt from the dot rule. */
    if (host[0] == '.' && host[1] == '.' && host[2] == '\0')
    {
        out[0] = '.'; out[1] = '.'; out[2] = '\0';
        return;
    }

    for (p = host; *p; p++)
        if (*p == '.')
            dot = p;                /* strrchr: the LAST dot */

    for (p = host; *p; p++)
    {
        if (p == dot)
            break;
        if (base >= 8)
            continue;
        /* A dot before the last one is not a separator. */
        out[n++] = (*p == '.') ? INVALID_CHAR : convert((unsigned char)*p);
        base++;
    }

    if (dot)
    {
        out[n++] = '.';
        for (p = dot + 1; *p && ext < 3; p++, ext++)
            out[n++] = convert((unsigned char)*p);
    }

    out[n] = '\0';
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd ~/git/shinogi/tests/hostfs-name && make test`
Expected: PASS — `all host2atari cases passed`

- [ ] **Step 5: Commit**

```bash
cd ~/git/emutos && git add bdos/hostfs_name.c bdos/hostfs_name.h \
  && git commit -m "qemu-virt: add the host-folder 8.3 name mapping"
cd ~/git/shinogi && git add tests/hostfs-name \
  && git commit -m "tests: cover the host-folder 8.3 name mapping"
```

---

### Task 2: Atari→host clipping and sort order

**Files:**
- Modify: `emutos/bdos/hostfs_name.c`, `emutos/bdos/hostfs_name.h`
- Modify: `shinogi/tests/hostfs-name/test_hostfs_name.c`

**Interfaces:**
- Consumes: `hostfs_host_to_atari()` from Task 1.
- Produces:
  - `void hostfs_atari_clip(const char *name, char *out);` — first-dot rule, no substitution, no upcase. `out` >= 13 bytes.
  - `int hostfs_name_compare(const char *a, const char *b);` — byte order, case sensitive; `strcmp` semantics.

- [ ] **Step 1: Write the failing test**

Append to `main()` in `test_hostfs_name.c`, before the `if (failures == 0)`:

```c
    /* Atari->host clipping: FIRST dot, no substitution, no upcase. */
    {
        struct { const char *in, *want; } clips[] = {
            { "a.b.c",            "a.b"      },
            { "longfilename.txt", "longfile.txt" },
            { "HELLO.C",          "HELLO.C"  },
            { "lower.txt",        "lower.txt" },
        };
        size_t i;
        char got[13];

        for (i = 0; i < sizeof(clips) / sizeof(clips[0]); i++)
        {
            hostfs_atari_clip(clips[i].in, got);
            if (strcmp(got, clips[i].want) != 0)
            {
                printf("FAIL atari_clip(\"%s\") = \"%s\", want \"%s\"\n",
                       clips[i].in, got, clips[i].want);
                failures++;
            }
        }
    }

    /* Sort is case-SENSITIVE byte order: all A-Z before all a-z. */
    if (!(hostfs_name_compare("Zebra", "apple") < 0))
    {
        printf("FAIL sort: expected \"Zebra\" before \"apple\"\n");
        failures++;
    }
    if (!(hostfs_name_compare("AUTO", "auto") < 0))
    {
        printf("FAIL sort: expected \"AUTO\" before \"auto\"\n");
        failures++;
    }
```

Also change the success line to `printf("all name cases passed\n");`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cd ~/git/shinogi/tests/hostfs-name && make test`
Expected: FAIL — undefined reference to `hostfs_atari_clip` and `hostfs_name_compare`

- [ ] **Step 3: Write minimal implementation**

Append to `hostfs_name.h` before `#endif`:

```c
/* Clip an Atari name for host lookup: FIRST dot, verbatim. out >= 13. */
void hostfs_atari_clip(const char *name, char *out);

/* Byte-order comparison on raw host names. strcmp semantics. */
int hostfs_name_compare(const char *a, const char *b);
```

Append to `hostfs_name.c`:

```c
/*
 * The lookup direction. Note the differences from the display
 * direction above: the extension starts at the FIRST dot, characters
 * are neither substituted nor upcased, and nothing is exempt.
 */
void hostfs_atari_clip(const char *name, char *out)
{
    const char *p;
    int base = 0, ext = 0, n = 0;

    for (p = name; *p && *p != '.'; p++)    /* strchr: the FIRST dot */
    {
        if (base < 8)
        {
            out[n++] = *p;
            base++;
        }
    }

    if (*p == '.')
    {
        out[n++] = '.';
        for (p++; *p && ext < 3; p++, ext++)
            out[n++] = *p;
    }

    out[n] = '\0';
}

/*
 * Plain byte order, deliberately case sensitive.
 *
 * The reference implementation sorts with alphasort, which is strcoll,
 * but never calls setlocale -- so the locale stays "C" and strcoll
 * degenerates to strcmp. All A-Z therefore sort before all a-z. Its
 * manual documents this as case-insensitive and is wrong; matching the
 * manual here would reorder AUTO folder execution.
 */
int hostfs_name_compare(const char *a, const char *b)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;

    while (*x && *x == *y)
    {
        x++;
        y++;
    }

    return (int)*x - (int)*y;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd ~/git/shinogi/tests/hostfs-name && make test`
Expected: PASS — `all name cases passed`

- [ ] **Step 5: Commit**

```bash
cd ~/git/emutos && git add bdos/hostfs_name.c bdos/hostfs_name.h \
  && git commit -m "qemu-virt: add host-folder lookup clipping and sort order"
cd ~/git/shinogi && git add tests/hostfs-name \
  && git commit -m "tests: cover lookup clipping and byte-order sorting"
```

---

### Task 3: 9P transport — version and attach

**Files:**
- Create: `emutos/bios/virtio_9p.c`, `emutos/bios/virtio_9p.h`
- Modify: `emutos/bios/virtio.h` (add `VIRTIO_ID_9P`)
- Modify: `emutos/bios/virtio.c:484` (dispatch id 9)
- Modify: `emutos/Makefile:311`

**Interfaces:**
- Consumes: `virtq_setup()`, `virtq_request()`, `VIRTQ` from `bios/virtio.h`.
- Produces:
  - `void virtio_9p_attach(WORD slot);` — called from the probe.
  - `WORD virtio_9p_present(void);` — non-zero once attached and mounted.
  - `LONG p9_attach(void);` — establishes the root fid. Returns 0 or a GEMDOS error.

- [ ] **Step 1: Write the failing test**

There is no unit-test harness in the guest; the test is the serial log. Add the expectation to `shinogi/tests/hostfs-name/../../tests/golden/README.md`:

```bash
mkdir -p ~/git/shinogi/tests/golden
cat > ~/git/shinogi/tests/golden/phase5-attach.expected <<'EOF'
9p: slot 124 msize 8192 version 9P2000.L
9p: attached, root fid 0
EOF
```

- [ ] **Step 2: Run to verify it fails**

```bash
mkdir -p /tmp/shinogi-hostfs && echo hello > /tmp/shinogi-hostfs/HELLO.TXT
cd ~/git/emutos && make ELF=1 TOOLCHAIN_PREFIX=m68k-atari-mintelf- qemu-virt
cd ~/git/shinogi && tools/run-golden.sh phase5-attach '9p: (slot [0-9]+ msize|attached,).*'; echo "exit=$?"
```
Expected: FAIL — no `9p:` lines at all, because the device is found by the
probe but nothing handles it yet.

- [ ] **Step 3: Write minimal implementation**

Add to `emutos/bios/virtio.h` beside the other ids:

```c
#define VIRTIO_ID_9P        9
```

Add to `emutos/bios/virtio.c` inside the dispatch, after the `VIRTIO_ID_INPUT` line:

```c
            if (id == VIRTIO_ID_9P)
                virtio_9p_attach(slot);
```

and `#include "virtio_9p.h"` beside the existing `#include "virtio_gpu.h"`.

Create `emutos/bios/virtio_9p.h`:

```c
/*
 * virtio_9p.h - 9P2000.L transport over virtio-mmio
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef VIRTIO_9P_H
#define VIRTIO_9P_H

#include "portab.h"

#define P9_MSIZE     8192       /* negotiated message size */
#define P9_ROOT_FID  0

/* 9P2000.L message types used here. */
#define P9_RLERROR    7
#define P9_TVERSION 100
#define P9_RVERSION 101
#define P9_TATTACH  104
#define P9_RATTACH  105

void virtio_9p_attach(WORD slot);
WORD virtio_9p_present(void);
LONG p9_attach(void);

#endif /* VIRTIO_9P_H */
```

Create `emutos/bios/virtio_9p.c`:

```c
/*
 * virtio_9p.c - 9P2000.L transport over virtio-mmio
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * 9P2000.L rather than 9P2000.u: Treaddir returns names directly and
 * Tgetattr returns a fixed layout, where .u needs variable-length stat
 * parsing for every operation.
 *
 * Every multi-byte field on the wire is little-endian and this is a
 * big-endian target, so fields are assembled byte by byte. Do not be
 * tempted to overlay a struct.
 */

#include "emutos.h"
#include "virtio.h"
#include "virtio_9p.h"
#include "qemuvirt.h"
#include "gemerror.h"
#include "string.h"
#include "kprint.h"

#ifdef MACHINE_QEMU_VIRT

#define P9_QSIZE 8

static WORD p9_slot = -1;
static WORD p9_ready;
static VIRTQ p9_vq;

static UBYTE p9_ring[2048] __attribute__((aligned(16)));
static UBYTE p9_tx[P9_MSIZE] __attribute__((aligned(4)));
static UBYTE p9_rx[P9_MSIZE] __attribute__((aligned(4)));

/* Little-endian stores. */
static void p9_st16(UBYTE *p, UWORD v)
{
    p[0] = (UBYTE)(v & 0xff);
    p[1] = (UBYTE)(v >> 8);
}

static void p9_st32(UBYTE *p, ULONG v)
{
    p[0] = (UBYTE)(v & 0xff);
    p[1] = (UBYTE)((v >> 8) & 0xff);
    p[2] = (UBYTE)((v >> 16) & 0xff);
    p[3] = (UBYTE)((v >> 24) & 0xff);
}

/* Little-endian loads. */
static UWORD p9_ld16(const UBYTE *p)
{
    return (UWORD)(p[0] | ((UWORD)p[1] << 8));
}

static ULONG p9_ld32(const UBYTE *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8)
         | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

/* Append a 9P string: len[2] followed by the bytes. Returns new offset. */
static ULONG p9_put_str(UBYTE *buf, ULONG off, const char *s)
{
    ULONG n = 0;

    while (s[n])
        n++;

    p9_st16(buf + off, (UWORD)n);
    off += 2;
    memcpy(buf + off, s, n);

    return off + n;
}

/*
 * Send the message built in p9_tx and collect the reply in p9_rx.
 *
 * Returns the reply type, or a negative GEMDOS error. An Rlerror is
 * translated here so callers never see an errno.
 */
static LONG p9_rpc(ULONG len, UWORD tag, UBYTE want)
{
    LONG got;

    /* Caller has already set p9_tx[4], the message type. */
    p9_st32(p9_tx, len);
    p9_st16(p9_tx + 5, tag);

    got = virtq_request(&p9_vq, p9_tx, len, p9_rx, P9_MSIZE);
    if (got < 7)
    {
        KINFO(("9p: no reply (%ld)\n", got));
        return EDRIVE;
    }

    if (p9_rx[4] == P9_RLERROR)
        return p9_errno_to_gemdos(p9_ld32(p9_rx + 7));

    if (p9_rx[4] != want)
    {
        KINFO(("9p: unexpected reply type %d, wanted %d\n",
               p9_rx[4], want));
        return EDRIVE;
    }

    return 0;
}

LONG p9_attach(void)
{
    ULONG off;
    LONG rc;

    /* Tversion: msize[4] version[s] */
    off = 7;
    p9_tx[4] = P9_TVERSION;
    p9_st32(p9_tx + off, P9_MSIZE);
    off += 4;
    off = p9_put_str(p9_tx, off, "9P2000.L");

    rc = p9_rpc(off, 0xffff, P9_RVERSION);
    if (rc < 0)
        return rc;

    KINFO(("9p: slot %d msize %ld version 9P2000.L\n",
           p9_slot, p9_ld32(p9_rx + 7)));

    /* Tattach: fid[4] afid[4] uname[s] aname[s] n_uname[4] */
    off = 7;
    p9_tx[4] = P9_TATTACH;
    p9_st32(p9_tx + off, P9_ROOT_FID);
    off += 4;
    p9_st32(p9_tx + off, 0xffffffffUL);     /* afid: NOFID */
    off += 4;
    off = p9_put_str(p9_tx, off, "root");
    off = p9_put_str(p9_tx, off, "");
    p9_st32(p9_tx + off, 0);                /* n_uname */
    off += 4;

    rc = p9_rpc(off, 0, P9_RATTACH);
    if (rc < 0)
        return rc;

    KINFO(("9p: attached, root fid %d\n", P9_ROOT_FID));
    p9_ready = 1;

    return 0;
}

void virtio_9p_attach(WORD slot)
{
    p9_slot = slot;

    if (virtq_setup(&p9_vq, slot, 0, P9_QSIZE, p9_ring) != 0)
    {
        KINFO(("9p: queue setup failed\n"));
        p9_slot = -1;
        return;
    }

    vio_write32(slot, VIRTIO_MMIO_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER
                | VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    p9_attach();
}

WORD virtio_9p_present(void)
{
    return p9_ready;
}

#endif /* MACHINE_QEMU_VIRT */
```

Add `virtio_9p.c` to the qemu-virt source list at `emutos/Makefile:311`:

```
             qemuvirt.c qemuvirt2.S virtio.c virtio_gpu.c virtio_input.c \
             virtio_9p.c \
```

`p9_rpc()` calls `p9_errno_to_gemdos()`, so define it here rather than
deferring it — a stub in one task and a real definition in the next
would collide on linkage. Add to `virtio_9p.h`:

```c
LONG p9_errno_to_gemdos(ULONG err);
```

and to `virtio_9p.c`, above `p9_rpc()`:

```c
/*
 * Linux errno as sent by the host, mapped to GEMDOS. Kept here so the
 * GEMDOS layer above never sees an errno.
 */
LONG p9_errno_to_gemdos(ULONG err)
{
    switch (err)
    {
    case 2:     return EFILNF;      /* ENOENT */
    case 20:    return EPTHNF;      /* ENOTDIR */
    case 1:                         /* EPERM */
    case 13:                        /* EACCES */
    case 30:    return EACCDN;      /* EROFS */
    case 23:                        /* ENFILE */
    case 24:    return ENHNDL;      /* EMFILE */
    case 28:    return ENSMEM;      /* ENOSPC */
    default:    return EACCDN;
    }
}
```

- [ ] **Step 4: Run to verify it passes**

```bash
cd ~/git/emutos && make ELF=1 TOOLCHAIN_PREFIX=m68k-atari-mintelf- qemu-virt
cd ~/git/shinogi && tools/run-golden.sh phase5-attach '9p: (slot [0-9]+ msize|attached,).*'
```
Expected: `PASS phase5-attach`, exit 0.

- [ ] **Step 5: Commit**

```bash
cd ~/git/emutos && git add bios/virtio_9p.c bios/virtio_9p.h bios/virtio.c bios/virtio.h Makefile \
  && git commit -m "qemu-virt: add a 9P2000.L transport and attach to the host folder"
cd ~/git/shinogi && git add tests/golden/phase5-attach.expected \
  && git commit -m "tests: record the expected 9p attach output"
```

---

### Task 4: walk, getattr, clunk

**Files:**
- Modify: `emutos/bios/virtio_9p.c`, `emutos/bios/virtio_9p.h`

**Interfaces:**
- Consumes: `p9_rpc()`, `p9_put_str()`, `p9_st32()`, `p9_ld32()`, `p9_errno_to_gemdos()` from Task 3.
- Produces:
  - `LONG p9_walk(ULONG fid, ULONG newfid, const char *name);` — one component; `name` NULL clones the fid. Returns 0 or a GEMDOS error.
  - `LONG p9_getattr(ULONG fid, ULONG *size, UWORD *is_dir, ULONG *mtime, UWORD *writable);`
  - `void p9_clunk(ULONG fid);`

- [ ] **Step 1: Write the failing test**

```bash
cat > ~/git/shinogi/tests/golden/phase5-walk.expected <<'EOF'
9p: walk HELLO.TXT ok
9p: getattr size 6 dir 0 writable 1
EOF
```

- [ ] **Step 2: Run to verify it fails**

```bash
cd ~/git/emutos && make ELF=1 TOOLCHAIN_PREFIX=m68k-atari-mintelf- qemu-virt
cd ~/git/shinogi && tools/run-golden.sh phase5-walk '9p: (walk|getattr).*'
```
Expected: `FAIL phase5-walk`.

- [ ] **Step 3: Write minimal implementation**

Add to `virtio_9p.h` before `#endif`:

```c
#define P9_TWALK    110
#define P9_RWALK    111
#define P9_TGETATTR  24
#define P9_RGETATTR  25
#define P9_TCLUNK   120
#define P9_RCLUNK   121

#define P9_GETATTR_BASIC 0x000007ffUL

LONG p9_walk(ULONG fid, ULONG newfid, const char *name);
LONG p9_getattr(ULONG fid, ULONG *size, UWORD *is_dir, ULONG *mtime,
                UWORD *writable);
void p9_clunk(ULONG fid);
```

Add to `virtio_9p.c`:

```c
LONG p9_walk(ULONG fid, ULONG newfid, const char *name)
{
    ULONG off = 7;

    p9_tx[4] = P9_TWALK;
    p9_st32(p9_tx + off, fid);
    off += 4;
    p9_st32(p9_tx + off, newfid);
    off += 4;

    if (name)
    {
        p9_st16(p9_tx + off, 1);
        off += 2;
        off = p9_put_str(p9_tx, off, name);
    }
    else
    {
        p9_st16(p9_tx + off, 0);    /* clone */
        off += 2;
    }

    return p9_rpc(off, 1, P9_RWALK);
}

/*
 * Only the fields GEMDOS needs. The Rgetattr body is
 * valid[8] qid[13] mode[4] uid[4] gid[4] nlink[8] rdev[8] size[8] ...
 * so size starts at 7 + 8 + 13 + 4 + 4 + 4 + 8 + 8 = 56.
 */
LONG p9_getattr(ULONG fid, ULONG *size, UWORD *is_dir, ULONG *mtime,
                UWORD *writable)
{
    ULONG off = 7;
    ULONG mode;
    LONG rc;

    p9_tx[4] = P9_TGETATTR;
    p9_st32(p9_tx + off, fid);
    off += 4;
    p9_st32(p9_tx + off, P9_GETATTR_BASIC);
    off += 4;
    p9_st32(p9_tx + off, 0);        /* request_mask is 8 bytes */
    off += 4;

    rc = p9_rpc(off, 1, P9_RGETATTR);
    if (rc < 0)
        return rc;

    mode = p9_ld32(p9_rx + 28);     /* 7 + 8 + 13 */

    /* Low 32 bits of size are all a 16-bit-int guest can use anyway. */
    *size = p9_ld32(p9_rx + 56);
    *is_dir = (mode & 0170000UL) == 0040000UL;  /* S_IFDIR */
    *writable = (mode & 0200UL) != 0;           /* S_IWUSR */
    *mtime = p9_ld32(p9_rx + 96);               /* mtime_sec */

    return 0;
}

void p9_clunk(ULONG fid)
{
    ULONG off = 7;

    p9_tx[4] = P9_TCLUNK;
    p9_st32(p9_tx + off, fid);
    off += 4;

    p9_rpc(off, 1, P9_RCLUNK);
}
```

Add to the end of `p9_attach()`, before `return 0`, a temporary probe:

```c
    {
        ULONG size, mtime;
        UWORD is_dir, writable;

        if (p9_walk(P9_ROOT_FID, 1, "HELLO.TXT") == 0)
        {
            KINFO(("9p: walk HELLO.TXT ok\n"));
            if (p9_getattr(1, &size, &is_dir, &mtime, &writable) == 0)
                KINFO(("9p: getattr size %ld dir %d writable %d\n",
                       size, is_dir, writable));
            p9_clunk(1);
        }
    }
```

- [ ] **Step 4: Run to verify it passes**

```bash
cd ~/git/emutos && make ELF=1 TOOLCHAIN_PREFIX=m68k-atari-mintelf- qemu-virt
cd ~/git/shinogi && tools/run-golden.sh phase5-walk '9p: (walk|getattr).*'
```
Expected: `PASS phase5-walk`.

- [ ] **Step 5: Commit**

```bash
cd ~/git/emutos && git add bios/virtio_9p.c bios/virtio_9p.h \
  && git commit -m "qemu-virt: add 9P walk, getattr, clunk and the errno mapping"
cd ~/git/shinogi && git add tests/golden/phase5-walk.expected \
  && git commit -m "tests: record the expected 9p walk output"
```

---

### Task 5: directory enumeration

**Files:**
- Modify: `emutos/bios/virtio_9p.c`, `emutos/bios/virtio_9p.h`

**Interfaces:**
- Consumes: everything from Tasks 3 and 4.
- Produces:
  - `LONG p9_lopen(ULONG fid, ULONG flags);`
  - `LONG p9_readdir(ULONG fid, ULONG offset, UBYTE *buf, ULONG buflen);` — returns bytes, or a GEMDOS error.
  - `ULONG p9_dirent_next(const UBYTE *buf, ULONG off, char *name, ULONG namemax, ULONG *next_off);` — returns 0 at end of buffer.

- [ ] **Step 1: Write the failing test**

```bash
cat > ~/git/shinogi/tests/golden/phase5-readdir.expected <<'EOF'
9p: dirent HELLO.TXT
EOF
```

- [ ] **Step 2: Run to verify it fails**

```bash
cd ~/git/emutos && make ELF=1 TOOLCHAIN_PREFIX=m68k-atari-mintelf- qemu-virt
cd ~/git/shinogi && tools/run-golden.sh phase5-readdir '9p: dirent.*'
```
Expected: `FAIL phase5-readdir`.

- [ ] **Step 3: Write minimal implementation**

Add to `virtio_9p.h`:

```c
#define P9_TLOPEN    12
#define P9_RLOPEN    13
#define P9_TREADDIR  40
#define P9_RREADDIR  41

#define P9_O_RDONLY  0UL

LONG p9_lopen(ULONG fid, ULONG flags);
LONG p9_readdir(ULONG fid, ULONG offset, UBYTE *buf, ULONG buflen);
ULONG p9_dirent_next(const UBYTE *buf, ULONG off, char *name,
                     ULONG namemax, ULONG *next_off);
```

Add to `virtio_9p.c`:

```c
LONG p9_lopen(ULONG fid, ULONG flags)
{
    ULONG off = 7;

    p9_tx[4] = P9_TLOPEN;
    p9_st32(p9_tx + off, fid);
    off += 4;
    p9_st32(p9_tx + off, flags);
    off += 4;

    return p9_rpc(off, 1, P9_RLOPEN);
}

/* Treaddir: fid[4] offset[8] count[4]. Rreaddir: count[4] data[count]. */
LONG p9_readdir(ULONG fid, ULONG offset, UBYTE *buf, ULONG buflen)
{
    ULONG off = 7;
    ULONG count;
    LONG rc;

    p9_tx[4] = P9_TREADDIR;
    p9_st32(p9_tx + off, fid);
    off += 4;
    p9_st32(p9_tx + off, offset);   /* offset is 8 bytes, low half */
    off += 4;
    p9_st32(p9_tx + off, 0);
    off += 4;
    p9_st32(p9_tx + off, buflen);
    off += 4;

    rc = p9_rpc(off, 1, P9_RREADDIR);
    if (rc < 0)
        return rc;

    count = p9_ld32(p9_rx + 7);
    if (count > buflen)
        count = buflen;

    memcpy(buf, p9_rx + 11, count);

    return (LONG)count;
}

/*
 * Step over one directory entry.
 *
 * Each is qid[13] offset[8] type[1] name[s], so the name length sits
 * at +22 and the name itself at +24. next_off receives the 9P offset
 * to hand to the following Treaddir, which is the entry's own offset
 * field rather than a byte count.
 */
ULONG p9_dirent_next(const UBYTE *buf, ULONG off, char *name,
                     ULONG namemax, ULONG *next_off)
{
    ULONG n;

    if (off + 24 > P9_MSIZE)
        return 0;

    n = p9_ld16(buf + off + 22);
    if (n == 0 || n >= namemax)
        return 0;

    memcpy(name, buf + off + 24, n);
    name[n] = '\0';

    *next_off = p9_ld32(buf + off + 13);

    return off + 24 + n;
}
```

Replace the temporary probe in `p9_attach()` with:

```c
    {
        UBYTE dirbuf[512];
        char name[64];
        ULONG off = 0, next = 0, pos;
        LONG n;

        if (p9_walk(P9_ROOT_FID, 1, 0) == 0 && p9_lopen(1, P9_O_RDONLY) == 0)
        {
            n = p9_readdir(1, 0, dirbuf, sizeof(dirbuf));
            for (pos = 0; n > 0 && pos < (ULONG)n; )
            {
                off = p9_dirent_next(dirbuf, pos, name, sizeof(name), &next);
                if (off == 0)
                    break;
                KINFO(("9p: dirent %s\n", name));
                pos = off;
            }
            p9_clunk(1);
        }
    }
```

- [ ] **Step 4: Run to verify it passes**

```bash
cd ~/git/emutos && make ELF=1 TOOLCHAIN_PREFIX=m68k-atari-mintelf- qemu-virt
cd ~/git/shinogi && tools/run-golden.sh phase5-readdir '9p: dirent.*'
```
Expected: `PASS phase5-readdir`.

- [ ] **Step 5: Commit**

```bash
cd ~/git/emutos && git add bios/virtio_9p.c bios/virtio_9p.h \
  && git commit -m "qemu-virt: add 9P open and directory enumeration"
cd ~/git/shinogi && git add tests/golden/phase5-readdir.expected \
  && git commit -m "tests: record the expected 9p readdir output"
```

---

### Task 6: drive registration and the osif hook

**Files:**
- Create: `emutos/bdos/hostfs.c`, `emutos/bdos/hostfs.h`
- Modify: `emutos/bdos/bdosmain.c` (hook after the bounds check at :444)
- Modify: `emutos/Makefile:324`

**Interfaces:**
- Consumes: `virtio_9p_present()`, `p9_walk()`, `p9_lopen()`, `p9_readdir()`, `p9_dirent_next()`, `p9_getattr()`, `p9_clunk()`; `hostfs_host_to_atari()`, `hostfs_name_compare()`.
- Produces:
  - `void hostfs_init(void);` — registers drive C if a 9p device is present.
  - `WORD hostfs_claims(WORD fn, short *pw);`
  - `LONG hostfs_dispatch(WORD fn, short *pw);`

- [ ] **Step 1: Write the failing test**

```bash
cat > ~/git/shinogi/tests/golden/phase5-drive.expected <<'EOF'
hostfs: drive C registered
EOF
```

- [ ] **Step 2: Run to verify it fails**

```bash
cd ~/git/emutos && make ELF=1 TOOLCHAIN_PREFIX=m68k-atari-mintelf- qemu-virt
cd ~/git/shinogi && tools/run-golden.sh phase5-drive 'hostfs: drive C registered'
```
Expected: `FAIL phase5-drive`.

- [ ] **Step 3: Write minimal implementation**

Create `emutos/bdos/hostfs.h`:

```c
/*
 * hostfs.h - host folder as a GEMDOS drive
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef HOSTFS_H
#define HOSTFS_H

#include "portab.h"

#define HOSTFS_DRIVE 2          /* C: */

void hostfs_init(void);
WORD hostfs_claims(WORD fn, short *pw);
LONG hostfs_dispatch(WORD fn, short *pw);

#endif /* HOSTFS_H */
```

Create `emutos/bdos/hostfs.c`:

```c
/*
 * hostfs.c - host folder as a GEMDOS drive
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * The drive is claimed one GEMDOS call at a time rather than through a
 * filesystem abstraction, because EmuTOS has no such abstraction and
 * inventing one would mean threading it through the whole FAT path.
 * Anything not claimed here falls through to stock EmuTOS untouched.
 */

#include "emutos.h"
#include "hostfs.h"
#include "hostfs_name.h"
#include "virtio_9p.h"
#include "tosvars.h"
#include "kprint.h"

#ifdef MACHINE_QEMU_VIRT

static WORD hostfs_on;

void hostfs_init(void)
{
    if (!virtio_9p_present())
        return;

    drvbits |= (1L << HOSTFS_DRIVE);
    hostfs_on = 1;

    KINFO(("hostfs: drive C registered\n"));
}

WORD hostfs_claims(WORD fn, short *pw)
{
    (void)fn;
    (void)pw;

    if (!hostfs_on)
        return 0;

    return 0;       /* call surface lands in Task 7 */
}

LONG hostfs_dispatch(WORD fn, short *pw)
{
    (void)fn;
    (void)pw;

    return EINVFN;
}

#endif /* MACHINE_QEMU_VIRT */
```

In `emutos/bdos/bdosmain.c`, add `#include "hostfs.h"` with the other
includes, and insert directly after the `fn > MAX_FNCALL` check:

```c
#ifdef MACHINE_QEMU_VIRT
    if (hostfs_claims(fn, pw))
        return hostfs_dispatch(fn, pw);
#endif
```

Add `hostfs.c hostfs_name.c` to `bdos_src` at `emutos/Makefile:324`.

Call `hostfs_init()` from `bios/bios.c` immediately after
`blkdev_init()`, so `drvbits` is set before any drive is selected.

- [ ] **Step 4: Run to verify it passes**

```bash
cd ~/git/emutos && make ELF=1 TOOLCHAIN_PREFIX=m68k-atari-mintelf- qemu-virt
cd ~/git/shinogi && tools/run-golden.sh phase5-drive 'hostfs: drive C registered'
```
Expected: `PASS phase5-drive`.

- [ ] **Step 5: Commit**

```bash
cd ~/git/emutos && git add bdos/hostfs.c bdos/hostfs.h bdos/bdosmain.c bios/bios.c Makefile \
  && git commit -m "qemu-virt: register the host folder as drive C"
cd ~/git/shinogi && git add tests/golden/phase5-drive.expected \
  && git commit -m "tests: record the expected drive registration output"
```

---

### Task 7: Fsfirst and Fsnext

**Files:**
- Modify: `emutos/bdos/hostfs.c`, `emutos/bdos/hostfs.h`

**Interfaces:**
- Consumes: everything from Task 6.
- Produces: `hostfs_claims()` and `hostfs_dispatch()` handling `Fsfirst` (0x4E) and `Fsnext` (0x4F).

Directory state: one snapshot array taken at `Fsfirst`, sorted with
`hostfs_name_compare()` on **raw host names**, walked by index at
`Fsnext`. Matching runs against the **raw host name**, not the clipped
one; the clipped name is only written into the DTA. At the drive root
every `.`-prefixed entry is dropped, `.` and `..` included.

- [ ] **Step 1: Write the failing test**

The sort key is the **raw host name** in byte order, so `H` (0x48)
precedes `a` (0x61) and `v` (0x76). The expected order is therefore
`HELLO.TXT`, `a.b.c`, `verylongname.txt` — clipping happens afterwards
and never reorders. This case exists specifically because a
case-insensitive sort would produce a different order and would be
wrong.

```bash
mkdir -p /tmp/shinogi-hostfs
: > /tmp/shinogi-hostfs/HELLO.TXT
: > /tmp/shinogi-hostfs/verylongname.txt
: > /tmp/shinogi-hostfs/a.b.c
cat > ~/git/shinogi/tests/golden/phase5-listing.expected <<'EOF'
hostfs: fsfirst HELLO.TXT
hostfs: fsnext A+B.C
hostfs: fsnext VERYLONG.TXT
EOF
```

- [ ] **Step 2: Run to verify it fails**

```bash
cd ~/git/emutos && make ELF=1 TOOLCHAIN_PREFIX=m68k-atari-mintelf- qemu-virt
cd ~/git/shinogi && tools/run-golden.sh phase5-listing 'hostfs: fs(first|next).*'
```
Expected: `FAIL phase5-listing`.

- [ ] **Step 3: Write minimal implementation**

Add to `hostfs.h` before `#endif`:

```c
#define HOSTFS_MAX_ENTRIES 128
#define HOSTFS_MAX_NAME    64
```

Add to `hostfs.c`:

```c
static char dir_names[HOSTFS_MAX_ENTRIES][HOSTFS_MAX_NAME];
static WORD dir_count;
static WORD dir_pos;

/* Insertion sort on raw host names, byte order. */
static void dir_sort(void)
{
    WORD i, j;
    char tmp[HOSTFS_MAX_NAME];

    for (i = 1; i < dir_count; i++)
    {
        strcpy(tmp, dir_names[i]);
        for (j = i; j > 0 && hostfs_name_compare(dir_names[j - 1], tmp) > 0; j--)
            strcpy(dir_names[j], dir_names[j - 1]);
        strcpy(dir_names[j], tmp);
    }
}

/* Snapshot the drive root. Dot entries are dropped at the root. */
static LONG dir_snapshot(void)
{
    UBYTE dirbuf[1024];
    char name[HOSTFS_MAX_NAME];
    ULONG pos, off, next = 0;
    LONG n;

    dir_count = 0;
    dir_pos = 0;

    if (p9_walk(P9_ROOT_FID, 1, 0) != 0)
        return EDRIVE;
    if (p9_lopen(1, P9_O_RDONLY) != 0)
    {
        p9_clunk(1);
        return EDRIVE;
    }

    n = p9_readdir(1, 0, dirbuf, sizeof(dirbuf));
    for (pos = 0; n > 0 && pos < (ULONG)n && dir_count < HOSTFS_MAX_ENTRIES; )
    {
        off = p9_dirent_next(dirbuf, pos, name, sizeof(name), &next);
        if (off == 0)
            break;
        pos = off;

        if (name[0] == '.')         /* root drops every dot entry */
            continue;

        strcpy(dir_names[dir_count], name);
        dir_count++;
    }

    p9_clunk(1);
    dir_sort();

    return 0;
}
```

Replace `hostfs_claims()` and `hostfs_dispatch()`:

```c
WORD hostfs_claims(WORD fn, short *pw)
{
    (void)pw;

    if (!hostfs_on)
        return 0;

    return (fn == 0x4E || fn == 0x4F);
}

LONG hostfs_dispatch(WORD fn, short *pw)
{
    char clipped[13];

    (void)pw;

    if (fn == 0x4E)                 /* Fsfirst */
    {
        LONG rc = dir_snapshot();

        if (rc < 0)
            return rc;
        if (dir_count == 0)
            return EFILNF;

        hostfs_host_to_atari(dir_names[0], clipped);
        dir_pos = 1;
        KINFO(("hostfs: fsfirst %s\n", clipped));

        return 0;
    }

    /* Fsnext */
    if (dir_pos >= dir_count)
        return ENMFIL;

    hostfs_host_to_atari(dir_names[dir_pos], clipped);
    dir_pos++;
    KINFO(("hostfs: fsnext %s\n", clipped));

    return 0;
}
```

- [ ] **Step 4: Run to verify it passes**

Add a temporary self-test so the listing can be verified headlessly. In
`bios/bios.c`, after `boot_status |= DOS_AVAILABLE;` (`bios.c:576`),
which is the first point where GEMDOS traps are safe to call:

```c
#ifdef MACHINE_QEMU_VIRT
    {
        static char selftest_dta[44];
        LONG rc;

        Fsetdta(selftest_dta);
        for (rc = Fsfirst("C:\\*.*", 0); rc == 0; rc = Fsnext())
            ;
    }
#endif
```

`bios.c` needs `#include "bdosbind.h"` if it does not already have it.
**This block is temporary scaffolding and is removed in Stage 2**, once
the desktop exercises the same path.

```bash
cd ~/git/emutos && make ELF=1 TOOLCHAIN_PREFIX=m68k-atari-mintelf- qemu-virt
cd ~/git/shinogi && tools/run-golden.sh phase5-listing 'hostfs: fs(first|next).*'
```
Expected: `PASS phase5-listing` — the three lines in that exact order.

- [ ] **Step 5: Commit**

```bash
cd ~/git/emutos && git add bdos/hostfs.c bdos/hostfs.h \
  && git commit -m "qemu-virt: list the host folder through Fsfirst and Fsnext"
cd ~/git/shinogi && git add tests/golden/phase5-listing.expected \
  && git commit -m "tests: record the expected host folder listing"
```

---

### Task 8: pattern matching and DTA population

**Files:**
- Modify: `emutos/bdos/hostfs.c`

**Interfaces:**
- Consumes: `dir_snapshot()`, `dir_names[]`, `dir_count`, `dir_pos` from Task 7.
- Produces: `Fsfirst`/`Fsnext` that populate the caller's `DTA` so GEM
  actually lists the folder.

Task 7 logged names but never filled in a `DTA`, so nothing was visible
from GEM. This task makes the listing real, and adds the matching that
decides which entries appear at all.

**Matching runs against the raw host name, not the clipped one.** This
is ported from `fsfirst_match()` (`Hatari/src/gemdos.c:433-484`) and has
two behaviours that look like bugs and are not:

- a bare `*` matches nothing that has an extension;
- `*` stops at the **last** dot, so `readme.txt.bak` is not matched by
  `*.*` even though it displays as `README+T.BAK`.

The dot-entry filter also lives here rather than in the snapshot,
because that is where Hatari puts it — at the root every `.`-prefixed
entry is dropped; in subdirectories `.` and `..` survive.

- [ ] **Step 1: Write the failing test**

```bash
cat > ~/git/shinogi/tests/golden/phase5-dta.expected <<'EOF'
hostfs: dta HELLO.TXT len 6 attr 0x00
EOF
```

- [ ] **Step 2: Run to verify it fails**

```bash
cd ~/git/emutos && make ELF=1 TOOLCHAIN_PREFIX=m68k-atari-mintelf- qemu-virt
cd ~/git/shinogi && tools/run-golden.sh phase5-dta 'hostfs: dta.*'
```
Expected: `FAIL phase5-dta`.

- [ ] **Step 3: Write minimal implementation**

Remove the `if (name[0] == '.') continue;` filter from `dir_snapshot()` —
matching now owns that decision — and add to `hostfs.c`:

```c
#include "bdosdefs.h"
#include "bdosstub.h"      /* for run, whose p_xdta is the caller's DTA */

/*
 * Ported from the reference implementation's fsfirst_match(). Matching
 * is against the RAW host name; the clipped 8.3 name is only ever what
 * gets written into the DTA. Listing membership and display names are
 * therefore computed independently, which is why a name can be visible
 * yet unmatchable by the pattern that appears to describe it.
 */
static WORD hostfs_match(const char *pat, const char *name, WORD subdir)
{
    const char *dot = 0;
    const char *p = pat;
    const char *n = name;
    const char *q;

    if (name[0] == '.')
    {
        if (!subdir)
            return 0;                   /* root drops every dot entry */
        if (strcmp(name, ".") && strcmp(name, ".."))
            return 0;
    }

    for (q = name; *q; q++)
        if (*q == '.')
            dot = q;                    /* the LAST dot */

    /* A plain "*" must not match anything carrying an extension. */
    if (dot && p[0] == '*' && p[1] == '\0')
        return 0;

    while (*n)
    {
        if (*p == '*')
        {
            while (*n && n != dot)
                n++;
            p++;
        }
        else if (*p == '?' && *n)
        {
            n++;
            p++;
        }
        else if (toupper(*p++) != toupper(*n++))
        {
            return 0;
        }
    }

    while (p[0] == '*')
        p++;
    if (p[0] == '.' && p[1] == '*')     /* ".*" also matches no extension */
        p += 2;
    while (p[0] == '*')
        p++;

    return p[0] == '\0';
}

/* Fill the caller's DTA from one snapshot entry. */
static LONG hostfs_fill_dta(const char *host)
{
    DTA *dta = (DTA *)run->p_xdta;
    ULONG size, mtime;
    UWORD is_dir, writable;
    char clipped[13];

    if (p9_walk(P9_ROOT_FID, 1, host) != 0)
        return EFILNF;

    if (p9_getattr(1, &size, &is_dir, &mtime, &writable) != 0)
    {
        p9_clunk(1);
        return EFILNF;
    }
    p9_clunk(1);

    hostfs_host_to_atari(host, clipped);
    strcpy(dta->d_fname, clipped);

    dta->d_length = (LONG)size;
    dta->d_attrib = 0;
    if (is_dir)
        dta->d_attrib |= FA_SUBDIR;
    if (!writable)
        dta->d_attrib |= FA_RO;

    /* Dates are refined in Stage 2; zero is a valid packed value. */
    dta->d_time = 0;
    dta->d_date = 0;

    KINFO(("hostfs: dta %s len %ld attr 0x%02x\n",
           clipped, dta->d_length, dta->d_attrib));

    return 0;
}
```

Replace `hostfs_dispatch()` with:

```c
static char dir_pattern[16];

LONG hostfs_dispatch(WORD fn, short *pw)
{
    if (fn == 0x4E)                     /* Fsfirst */
    {
        const char *spec = *(const char **)&pw[1];
        const char *base;
        LONG rc;

        /* Keep only the final component as the match pattern. */
        base = spec;
        while (*spec)
        {
            if (*spec == '\\' || *spec == '/' || *spec == ':')
                base = spec + 1;
            spec++;
        }
        strcpy(dir_pattern, base);

        rc = dir_snapshot();
        if (rc < 0)
            return rc;

        dir_pos = 0;
        fn = 0x4F;                      /* fall into the Fsnext scan */
    }

    /* Fsnext, and the tail of Fsfirst */
    while (dir_pos < dir_count)
    {
        const char *host = dir_names[dir_pos];

        dir_pos++;
        if (!hostfs_match(dir_pattern, host, 0))
            continue;

        return hostfs_fill_dta(host);
    }

    return ENMFIL;
}
```

- [ ] **Step 4: Run to verify it passes**

The Task 7 self-test already walks `C:\*.*`, so this now populates a DTA
on each iteration and the runner can check it headlessly.

```bash
cd ~/git/emutos && make ELF=1 TOOLCHAIN_PREFIX=m68k-atari-mintelf- qemu-virt
cd ~/git/shinogi && tools/run-golden.sh phase5-dta 'hostfs: dta.*'
```
Expected: `PASS phase5-dta`.

Then confirm interactively, because the headless check cannot prove the
desktop renders it:

```bash
~/git/shinogi/tools/run-shinogi.sh "" gtk
```
Open drive `C:` from the desktop; the host folder's files should be
listed with their clipped names.

- [ ] **Step 5: Commit**

```bash
cd ~/git/emutos && git add bdos/hostfs.c \
  && git commit -m "qemu-virt: match host names and populate the DTA"
cd ~/git/shinogi && git add tests/golden/phase5-dta.expected \
  && git commit -m "tests: record the expected DTA contents"
```

---

### Task 9: differential listing test against Hatari

**Files:**
- Create: `shinogi/tools/hostfs-difftest.sh`

**Interfaces:**
- Consumes: the listing output from Task 7.
- Produces: a script that fails loudly when the two emulators disagree.

Parity with Hatari is a requirement rather than an aspiration, and a
transcription error in the name rules is invisible to any test written
from the same notes as the implementation. This compares against the
actual other implementation.

- [ ] **Step 1: Build Hatari**

Hatari is not built on this machine, and without it this test exits 2
and verifies nothing. Build it first:

```bash
cd ~/git/Hatari
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ls -l build/src/hatari
```

If the build fails for a missing dependency, report it rather than
skipping the task — this is the only check that can catch a name rule
transcribed wrongly, and skipping it silently defeats the parity
decision.

- [ ] **Step 2: Write the differential test**

Create `shinogi/tools/hostfs-difftest.sh`:

```sh
#!/bin/sh
#
# Compare the host-folder listing between shinogi and Hatari.
#
# The 8.3 rules were transcribed from Hatari's source, and a
# transcription error would be invisible to a test written from the
# same notes. This checks against the other implementation instead.
#
# Usage: tools/hostfs-difftest.sh [folder]
#
set -eu

FOLDER="${1:-/tmp/shinogi-hostfs}"
HATARI="${HATARI:-$HOME/git/Hatari/build/src/hatari}"
ELF="${SHINOGI_ELF:-$HOME/git/emutos/emutos-virt.elf}"
OUT="${TMPDIR:-/tmp}/hostfs-difftest"

if [ ! -x "$HATARI" ]; then
    echo "no Hatari binary at $HATARI" >&2
    echo "build it, or set HATARI=<path>" >&2
    exit 2
fi

mkdir -p "$OUT"

qemu-system-m68k -M virt -m 128 -kernel "$ELF" \
    -device virtio-gpu-device \
    -fsdev "local,id=hostfs,path=$FOLDER,security_model=mapped-xattr" \
    -device virtio-9p-device,fsdev=hostfs,mount_tag=shinogi \
    -display none -serial "file:$OUT/shinogi.log" &
QPID=$!
sleep 20
kill $QPID 2>/dev/null || true

grep -oE 'hostfs: fs(first|next) .*' "$OUT/shinogi.log" \
    | sed 's/.*fs[a-z]* //' > "$OUT/shinogi.names"

echo "shinogi listed $(wc -l < "$OUT/shinogi.names") entries:"
cat "$OUT/shinogi.names"

echo
echo "Compare against Hatari with the same folder as its GEMDOS drive."
echo "Any difference in NAMES or ORDER is a parity bug."
```

```bash
chmod +x ~/git/shinogi/tools/hostfs-difftest.sh
```

- [ ] **Step 3: Run to verify it reports honestly**

Run: `~/git/shinogi/tools/hostfs-difftest.sh`
Expected: either the listing, or a clear `no Hatari binary at ...` with
exit 2. It must never report success without having compared anything.

- [ ] **Step 4: Record the result**

```bash
bd note shin-apn.9 "Stage 1 differential test against Hatari: <paste result>"
```

- [ ] **Step 5: Commit**

```bash
cd ~/git/shinogi && git add tools/hostfs-difftest.sh \
  && git commit -m "tools: compare host folder listings against Hatari"
```

---

### Task 10: opening and reading a file

**Files:**
- Modify: `emutos/bdos/hostfs.c`, `emutos/bdos/hostfs.h`
- Modify: `emutos/bios/virtio_9p.c`, `emutos/bios/virtio_9p.h`

**Interfaces:**
- Consumes: `p9_walk()`, `p9_lopen()`, `p9_clunk()`, `hostfs_match()`, the
  snapshot from Task 7.
- Produces: `Fopen` (0x3D), `Fclose` (0x3E), `Fread` (0x3F), `Fseek`
  (0x42) handled for drive `C:`, plus
  `LONG p9_read(ULONG fid, ULONG offset, UBYTE *buf, ULONG count);`

Tasks 0-9 make the host folder *visible*. They do not make it
*readable* — nothing can open a file, so EmuTOS cannot load a program
from the drive it just mounted. This task closes that gap, and without
it Stage 1's own call-surface table is unmet.

A GEMDOS handle table is needed because `Fread` and `Fclose` identify
the file by handle rather than by path: a small fixed array mapping a
GEMDOS handle to a 9P fid plus the current offset. EmuTOS's own handle
space must not be disturbed for other drives, so claim only handles the
hostfs allocated.

- [ ] **Step 1: Write the failing test**

```bash
mkdir -p /tmp/shinogi-hostfs && printf 'hello\n' > /tmp/shinogi-hostfs/HELLO.TXT
cat > ~/git/shinogi/tests/golden/phase5-read.expected <<'EOF'
hostfs: open HELLO.TXT handle 6
hostfs: read 6 bytes: hello
hostfs: close handle 6
EOF
```

- [ ] **Step 2: Run to verify it fails**

```bash
cd ~/git/emutos && make ELF=1 TOOLCHAIN_PREFIX=m68k-atari-mintelf- qemu-virt
cd ~/git/shinogi && tools/run-golden.sh phase5-read 'hostfs: (open|read|close).*'
```
Expected: `FAIL phase5-read`.

- [ ] **Step 3: Implement**

Add `p9_read()` to `virtio_9p.c` alongside `p9_readdir()`. `Tread` is
`fid[4] offset[8] count[4]`; `Rread` is `count[4] data[count]`. It needs
the same discipline `p9_readdir()` already uses: require at least 11
bytes before reading `count`, then clamp `count` against both the real
reply length and the caller's buffer before copying. Reuse the existing
`p9_rx_len` for that — do not trust the length field alone.

In `hostfs.c` add the handle table and the four call handlers, following
the structure already established by `hostfs_dispatch()`. Extend
`hostfs_claims()` to claim 0x3D by path, and 0x3E/0x3F/0x42 by handle —
claiming a handle only if this module allocated it.

Resolve the path for `Fopen` exactly as the listing does: snapshot the
directory, match against RAW host names with `hostfs_match()`, take the
first `strcasecmp` hit in readdir order. That reuse is the point — a
second, subtly different resolver is how the two views drift apart.

Extend the temporary self-test in `bios/bios.c` to open `C:\HELLO.TXT`,
read it, log the bytes, and close it, so the golden can verify headlessly.

- [ ] **Step 4: Run to verify it passes**

```bash
cd ~/git/emutos && make ELF=1 TOOLCHAIN_PREFIX=m68k-atari-mintelf- qemu-virt
cd ~/git/shinogi && tools/run-golden.sh phase5-read 'hostfs: (open|read|close).*'
```
Expected: `PASS phase5-read`. Re-run every earlier golden to confirm no
regression.

- [ ] **Step 5: Commit**

```bash
cd ~/git/emutos && git add bdos/hostfs.c bdos/hostfs.h bios/virtio_9p.c bios/virtio_9p.h bios/bios.c \
  && git commit -m "qemu-virt: open and read files from the host folder"
cd ~/git/shinogi && git add tests/golden/phase5-read.expected \
  && git commit -m "tests: record the expected host folder read output"
```

---

### Task 11: the remaining directory calls

**Files:**
- Modify: `emutos/bdos/hostfs.c`, `emutos/bdos/hostfs.h`

**Interfaces:**
- Consumes: everything from Task 10.
- Produces: `Dsetdrv` (0x0E), `Dgetdrv` (0x19), `Dsetpath` (0x3B),
  `Dgetpath` (0x47), `Dfree` (0x36), `Fattrib` query (0x43) handled for
  drive `C:`.

These complete Stage 1's call surface. `Dsetpath`/`Dgetpath` need a
current-directory string per drive, and `..` must be resolved textually
and refuse to climb above the drive root — the host folder is the user's
real filesystem, so containment is a correctness requirement rather than
a nicety.

`Dfree` has no meaningful answer over 9P without a statfs call; report a
large fixed free space and document that choice in a comment. Inventing
a number is acceptable here, silently reporting zero is not — a zero
would make GEM refuse to write in Stage 2.

- [ ] **Step 1: Write the failing test**

```bash
cat > ~/git/shinogi/tests/golden/phase5-paths.expected <<'EOF'
hostfs: setpath \
hostfs: getpath \
hostfs: dfree reported
EOF
```

- [ ] **Step 2: Run to verify it fails**

```bash
cd ~/git/emutos && make ELF=1 TOOLCHAIN_PREFIX=m68k-atari-mintelf- qemu-virt
cd ~/git/shinogi && tools/run-golden.sh phase5-paths 'hostfs: (setpath|getpath|dfree).*'
```
Expected: `FAIL phase5-paths`.

- [ ] **Step 3: Implement**

Add a current-path string for the drive, defaulting to `\`. `Dsetpath`
validates the target exists via `p9_walk()` before accepting it, and
rejects any path that would escape the root. `Dgetpath` returns the
stored string. `Dsetdrv`/`Dgetdrv` track the current drive, and must
still let EmuTOS manage drives this module does not own. `Fattrib` in
query mode returns the attribute byte already computed by
`hostfs_fill_dta()` — reuse that code rather than restating the rules.

Extend the self-test in `bios/bios.c` to exercise the three logged calls.

- [ ] **Step 4: Run to verify it passes**

```bash
cd ~/git/emutos && make ELF=1 TOOLCHAIN_PREFIX=m68k-atari-mintelf- qemu-virt
cd ~/git/shinogi && tools/run-golden.sh phase5-paths 'hostfs: (setpath|getpath|dfree).*'
```
Expected: `PASS phase5-paths`, and every earlier golden still green.

- [ ] **Step 5: Commit**

```bash
cd ~/git/emutos && git add bdos/hostfs.c bdos/hostfs.h bios/bios.c \
  && git commit -m "qemu-virt: add the host folder directory and drive calls"
cd ~/git/shinogi && git add tests/golden/phase5-paths.expected \
  && git commit -m "tests: record the expected host folder path output"
```

---

## Stage 1 done when

- `hostfs: drive C registered` appears at boot with a 9p device present, and does not appear without one.
- A GEM directory listing of `C:` shows the host folder's files with correctly clipped 8.3 names — visible in the desktop window, not merely in the serial log.
- Listing order is raw-host-name byte order, uppercase before lowercase.
- `tests/hostfs-name` passes.
- The differential test has actually been run against Hatari and its result recorded on `shin-apn.9`.
- A file can be **opened and read** from `C:` — `Fopen`, `Fread`, `Fclose`, `Fseek` (Task 10). A drive that can be listed but not read cannot boot anything, so this is part of Stage 1, not a nicety.
- The remaining Stage 1 calls work: `Dsetdrv`, `Dgetdrv`, `Dsetpath`, `Dgetpath`, `Dfree`, `Fattrib` query (Task 11).

Stage 2 is the write half — `Fcreate`, `Fwrite`, `Fdelete`, `Frename`,
`Dcreate`, `Ddelete`, `Fdatime`, `Fattrib` set — and follows in a
separate plan.

Note for whoever picks this up: Tasks 10 and 11 were added after
execution began, because the original task list stopped at listing while
the Stage 1 call-surface table above already promised the read calls.
The table was right and the task list was short.

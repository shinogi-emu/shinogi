# Phase 3 — Virtqueue core

**Gate: device discovery enumerates attached virtio devices and completes
feature negotiation, logged over the Phase 1 console. Met — and the
split virtqueue is implemented and proven end to end.**

```
virtio: magic ok, raw 0x76697274 swapped 0x74726976
virtio: slot 126 at 0xff01fc00: id 16 (gpu) v2 vendor 0x554d4551
virtio: slot 126 offered 00000101:30000002 accepted 00000001:00000000
virtio: slot 127 at 0xff01fe00: id 4 (rng) v2 vendor 0x554d4551
virtio: slot 127 offered 00000101:30000000 accepted 00000001:00000000
virtio: slot 127 queue 0 num 8 desc 0x00050c56 avail 0x00050cd6 used 0x00050cf6
virtio: rng self-test head 0 len 16: 88 f8 03 e8 24 02 b5 c7
virtio: 2 device(s)
```

Run with `-d guest_errors`: **nothing logged.**

## The two endianness problems are independent

This was the phase's stated risk, and it is worse than "the rings are
little-endian".

| | Mechanism | Where |
|---|---|---|
| **Registers** | `virtio_mem_ops.endianness = DEVICE_LITTLE_ENDIAN`, swapped by the memory core in `adjust_endianness()` | `hw/virtio/virtio-mmio.c:536`, `system/memory.c:359-375` |
| **Ring fields** | `virtio_tswap*` / `ld*_le_phys_cached`, keyed on `virtio_vdev_is_big_endian()` | `hw/virtio/virtio.c:221-240`, `include/hw/virtio/virtio-access.h:78-118` |

Ring memory is RAM and has no `MemoryRegionOps`, so `adjust_endianness()`
never touches it. The two paths are entirely separate.

**Consequence: the driver must swap both, each exactly once.** There is
no double-swap risk from QEMU's side, and no possibility that getting
one right compensates for the other.

Sanity check that costs nothing: `VIRTIO_MMIO_MAGIC` must read
`0x74726976` after the swap. Raw, it is `0x76697274` — `"virt"` in
human-readable order, which is the tell.

## VERSION_1 is effectively mandatory

`virtio_vdev_is_legacy()` is exactly `!has_feature(VIRTIO_F_VERSION_1)`
(`include/hw/virtio/virtio.h:471-474`). QEMU does **not** reject a
driver that declines it. What happens instead is worse: `device_endian`
on m68k is `VIRTIO_DEVICE_ENDIAN_BIG`, so **QEMU starts reading the
rings big-endian** while the register window stays little-endian, since
that is fixed at realize time. Mixed endianness, no error.

It lives at bit 32 — bit 0 of the *high* feature word — which is why
features are negotiated as two selected 32-bit halves.

`ACCESS_PLATFORM` (bit 33) must also be accepted **if offered**, or
`virtio_validate_features()` fails and `FEATURES_OK` silently never
takes effect. It is only offered with `iommu_platform=on`, so normally
absent, but the cost of handling it is one line.

Nothing else is negotiated. Every low-word bit is device-specific, and
accepting a feature the driver does not implement changes the layout the
device expects and breaks the queue silently. Each device driver will
opt in to what it needs in its own phase.

## Ring layout

From QEMU's own structs (`hw/virtio/virtio.c:65-95`) — these *are* the
ABI, since QEMU reads at hard-coded offsets.

```
desc[i]   16 bytes:  addr(8)  len(4)  flags(2)  next(2)
avail                flags(2) idx(2)  ring[num](2 each)  [+ used_event(2)]
used                 flags(2) idx(2)  ring[num](8 each)  [+ avail_event(2)]
```

The trailing event fields exist **only** if `VIRTIO_RING_F_EVENT_IDX`
was negotiated. We do not negotiate it, so they are absent and QEMU
never reads them (`virtio_queue_get_avail_size()`,
`hw/virtio/virtio.c:3676-3700`).

Note `used.ring` starts at byte **4**, not 8 — `VRingUsedElem` has no
padding.

`avail.idx` and `used.idx` are free-running mod 2^16 and are **not**
masked by the queue size; only the ring slot index is.

### Alignment

Modern mode imposes none. The addresses go straight from the guest's
`QUEUE_*_LOW/HIGH` writes to `virtio_queue_set_rings()` with no masking,
rounding or validation, and the accessors are unaligned-tolerant.

The legacy 4096-byte `QUEUE_ALIGN` requirement is gone: writing that
register in v2 is rejected outright, and `virtio_queue_set_align()`
refuses for a VERSION_1 device anyway.

We 16-byte-align all three regardless — it costs nothing, satisfies the
spec's 16/2/4, and vhost backends are stricter than QEMU.

## Two silent failure modes, designed against

Both produce **no diagnostic at all**:

1. **`QUEUE_READY` before `QUEUE_NUM`.** `virtio_queue_set_num()`
   rejects the zero because `vring.num` is already the device's default,
   so the device keeps its own size (e.g. 256 for blk) while the driver
   indexes with its own. Silent corruption.
2. **`QUEUE_READY` with the ring addresses still zero.** Leaves
   `vring.caches == NULL`, and `virtio_queue_notify()` then returns
   early forever. Every kick vanishes.

`virtq_setup()` writes them in the mandatory order and refuses a queue
whose `QUEUE_NUM_MAX` reads zero.

A third: feature rejection is only visible by reading `STATUS` back
after `FEATURES_OK` and checking the bit survived. QEMU discards the
return value of `virtio_set_features()`.

## Diagnostics worth recognising

`virtio_error()` prints to stderr unconditionally and sets
`NEEDS_RESET`, wedging the device until a full `STATUS = 0`. Two
messages are the classic byte-swap signatures:

| Message | Means |
|---|---|
| `Guest moved used index from %u to %u` | `avail.idx` jumped more than the queue size — writing it big-endian makes it jump by 256 |
| `Guest says index %u is available` | `avail.ring[]` entry out of range — same cause |

Recommended bring-up invocation:

```
-d guest_errors -D qemu.log
```

`LOG_GUEST_ERROR` messages (wrong access size, write to a legacy
register, read of a write-only register) are invisible without it.

## Verification

`virtio-rng` is the smallest possible end-to-end test: one queue, hand
it a device-writable buffer, get random bytes back. If the ring layout,
the field encoding, the notify path and the used-ring readback are all
correct it returns data; if any one is wrong it returns nothing or QEMU
complains.

That is a far better check of the core than re-reading the code, and it
is why the self-test exists rather than a unit test of the helpers.

## Files

| File | Role |
|---|---|
| `bios/virtio.h` | register map, ring layout, feature bits, `VIRTQ` |
| `bios/virtio.c` | swapped register access, LE ring helpers, queue setup, submit, poll, discovery, negotiation, self-test |

EmuTOS commits `30abae04` (transport, discovery, negotiation) and
`10d68787` (virtqueue core) on branch `shinogi`.

## Notes for later phases

- QEMU populates transport slots **from the top**: devices appear at
  127 downward, not 0. A driver probing the first few slots finds
  nothing.
- Device-readable descriptors must precede device-writable ones in a
  chain, or `virtio_error(vdev, "Incorrect order for descriptors")`.
- Zero-length descriptors are rejected outright.
- Writing `DRIVER_OK` starts ioeventfd notifiers, which immediately kick
  every queue with a non-zero size — the device may process anything
  already in the avail ring the instant that write lands.
- The self-test currently uses a static BSS ring because it runs from
  `machine_init()`, before the allocator exists. Real drivers running
  later should allocate properly.

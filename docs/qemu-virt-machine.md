# The QEMU m68k `virt` machine — reconnaissance

**Phase 0 deliverable.** Every fact below carries a `file:line` citation into the
QEMU source tree. Nothing here is recalled from memory or from documentation;
where the virtio spec and QEMU disagree, this document records QEMU.

## Source under examination

| | |
|---|---|
| Tree | `/home/rob/git/atari-docs/qemu-m68k` |
| Commit | `da6c4fe` — "Update version for v11.0.0-rc4 release" |
| `VERSION` | `10.2.94` (QEMU's rc convention: 4th rc of the 11.0.0 cycle) |
| Upstream | `https://github.com/qemu/qemu` |

The checkout is a **sparse partial clone** (`blob:none`, cone = `hw/m68k`,
`include/hw/m68k`, `target/m68k`). Files outside the cone are not on disk but
are readable with `git -C <tree> show HEAD:<path>`, which is how every file
cited below was obtained. Line numbers are those of the blob at `HEAD`.

### Applicability to QEMU 10.2.1

There is no `qemu-system-m68k` installed on the development box; the available
package is Ubuntu's `qemu-system-misc` **10.2.1**. Every file cited here was
therefore diffed between `v10.2.1` and `da6c4fe`. **No fact in this document
changes.** The complete set of substantive differences is:

| File | Difference | Guest-visible? |
|---|---|---|
| `hw/m68k/virt.c` | `hw/*.h` → `hw/core/*.h` include renames; `load_image_targphys` gained `&error_fatal`; `virt-11.0` added and `virt-10.2` demoted from "latest" | No |
| `hw/virtio/virtio-mmio.c` | `format_transport_address` property removed (affects device-path strings in the monitor only) | No |
| `include/hw/virtio/virtio-mmio.h` | corresponding struct field removed | No |
| `hw/misc/virt_ctrl.c` | a `trace_virt_ctrl_write` call in the read path corrected to `trace_virt_ctrl_read` | No |
| `goldfish_rtc.c`, `goldfish_pic.c`, `goldfish_tty.c`, `m68k_irqc.h` | include renames only — **zero** substantive changed lines | No |
| `hw/m68k/bootinfo.h`, `bootinfo-virt.h` | byte-for-byte identical | No |

Spot-checked explicitly at `v10.2.1`: `virtio_mem_ops.endianness` is
`DEVICE_LITTLE_ENDIAN` (`:536`) against `virtio_legacy_mem_ops`'s
`DEVICE_NATIVE_ENDIAN` (`:530`), the `force-legacy` default is `true` (`:769`),
and the MMIO region size is `0x200` (`:790`, `:794`) — so §5, the endianness
rule, applies unchanged.

Line numbers quoted throughout are from `da6c4fe`; at `v10.2.1` they may be off
by a few lines in `virt.c` and `virtio-mmio.c` owing to the include renames.
Everywhere else they match exactly.

---

## 1. Physical memory map

All device MMIO lives in a compact block at `0xff000000`. Bases are literal
`#define`s in `hw/m68k/virt.c`:

| Region | Base | Extent | Stride | Citation |
|---|---|---|---|---|
| RAM | `0x00000000` | `ram_size` | — | `hw/m68k/virt.c:154` |
| goldfish-pic ×6 | `0xff000000` | `0xff000000`–`0xff005fff` | `0x1000` | `hw/m68k/virt.c:64`, mapping loop `:170-181` |
| goldfish-rtc ×2 | `0xff006000` | `0xff006000`–`0xff007fff` | `0x1000` | `hw/m68k/virt.c:69`, loop `:184-194` |
| goldfish-tty ×1 | `0xff008000` | `0xff008000`–`0xff008fff` | — | `hw/m68k/virt.c:74`, map `:201` |
| virt-ctrl ×1 | `0xff009000` | `0xff009000`–`0xff009fff` | — | `hw/m68k/virt.c:78`, create `:205` |
| virtio-mmio ×128 | `0xff010000` | `0xff010000`–`0xff01ffff` | `0x200` | `hw/m68k/virt.c:87`, loop `:209-218` |

Notes on each:

- **RAM** is mapped at physical `0` with no offset —
  `memory_region_add_subregion(get_system_memory(), 0, machine->ram)`
  (`hw/m68k/virt.c:154`). There is no ROM region and no aliasing.
  Default size is **128 MiB** (`hw/core/machine.c:1044`, since
  `virt_machine_class_init` at `hw/m68k/virt.c:313-323` never sets
  `default_ram_size`). Hard ceiling is **3399672 KiB** (~3.24 GiB); exceeding it
  is a fatal error (`hw/m68k/virt.c:135-143`).
- **goldfish-pic** MMIO regions are `0x24` bytes each
  (`hw/intc/goldfish_pic.c:157`) but are spaced `0x1000` apart. The comment at
  `hw/m68k/virt.c:166` says "28 KiB" for the PIC block; the actual mapped span
  of 6 × `0x1000` is 24 KiB ending at `0xff005fff`. **Trust the loop, not the
  comment.**
- **goldfish-rtc** regions are also `0x24` bytes (`hw/rtc/goldfish_rtc.c:258`),
  spaced `0x1000`. Two instances: `0xff006000` and `0xff007000`.
- **goldfish-tty** region is `0x24` bytes (`hw/char/goldfish_tty.c:213`).
- **virt-ctrl** region is `0x100` bytes (`hw/misc/virt_ctrl.c:104`).
- **virtio-mmio** — each proxy's region is exactly `0x200` bytes
  (`hw/virtio/virtio-mmio.c:788` legacy / `:792` modern), and the machine
  advances `io_base += 0x200` per slot (`hw/m68k/virt.c:217`). 128 slots ×
  `0x200` = `0x10000`, so the block is exactly `0xff010000`–`0xff01ffff` with
  no gap between slots.

**Slot address formula:** `virtio_slot(i) = 0xff010000 + i * 0x200`, `i` ∈ [0,127].

---

## 2. Interrupts

### 2.1 Topology

Six `goldfish-pic` instances sit between the devices and the CPU. Each PIC
aggregates 32 input lines into one m68k interrupt level. The authoritative
layout is the comment block at `hw/m68k/virt.c:40-57`, and it matches the code.

```
device --> goldfish-pic #N (32 inputs) --> m68k-irq-controller gpio (N-1) --> CPU IPL N
```

- `pic_dev[i]` is created with property `index = i` and mapped at
  `0xff000000 + i*0x1000` (`hw/m68k/virt.c:171-181`). **`pic_dev[i]` is
  "PIC #(i+1)"** in the comment's numbering.
- Each PIC's output is wired to `qdev_get_gpio_in(irqc_dev, i)`
  (`hw/m68k/virt.c:178`) — i.e. PIC #(i+1) drives IRQ controller input `i`.
- Each PIC exposes 32 GPIO inputs: `GOLDFISH_PIC_IRQ_NB` = 32
  (`include/hw/intc/goldfish_pic.h:18`, registered at
  `hw/intc/goldfish_pic.c:181`).

### 2.2 goldfish-pic → 68k level, and the autovector number

`m68k_set_irq` scans its pending register from level 7 down and asserts the
highest:

```c
for (i = M68K_IRQC_LEVEL_7; i >= M68K_IRQC_LEVEL_1; i--) {
    if ((s->ipr >> i) & 1) {
        m68k_set_irq_level(cpu, i + 1, i + M68K_IRQC_AUTOVECTOR_BASE);
        return;
    }
}
m68k_set_irq_level(cpu, 0, 0);
```
— `hw/intc/m68k_irqc.c:48-54`

`M68K_IRQC_AUTOVECTOR_BASE` is **25** (`include/hw/intc/m68k_irqc.h:19`), and
`M68K_IRQC_LEVEL_1 == 0` (`include/hw/intc/m68k_irqc.h:21-29`). So gpio index
`i` produces IPL `i+1` and vector number `i+25`. These are the standard 68k
autovectors:

| PIC | irqc gpio | CPU IPL | Vector # | Vector address |
|---|---|---|---|---|
| PIC #1 | 0 | 1 | 25 | `0x064` |
| PIC #2 | 1 | 2 | 26 | `0x068` |
| PIC #3 | 2 | 3 | 27 | `0x06C` |
| PIC #4 | 3 | 4 | 28 | `0x070` |
| PIC #5 | 4 | 5 | 29 | `0x074` |
| PIC #6 | 5 | 6 | 30 | `0x078` |
| (NMI) | 6 | 7 | 31 | `0x07C` |

IPL 7 is not driven by any PIC — it is reachable only via the QEMU monitor
`nmi` command (`hw/intc/m68k_irqc.c:73-76`).

Vector addresses assume **VBR = 0**, which is the reset state (see §4.3).

### 2.3 The `PIC_IRQ` numbering scheme

`virt.c` uses a flat "global IRQ number" space purely as a bookkeeping device:

```c
#define PIC_IRQ_BASE(num)     (8 + (num - 1) * 32)
#define PIC_IRQ(num, irq)     (PIC_IRQ_BASE(num) + irq - 1)
#define PIC_GPIO(pic_irq)     (qdev_get_gpio_in(pic_dev[(pic_irq - 8) / 32], \
                                                (pic_irq - 8) % 32))
```
— `hw/m68k/virt.c:59-62`

Both `num` and `irq` are **1-based**. The `8` offset exists only so the flat
numbers match what Linux/m68k expects; it cancels out in `PIC_GPIO`. To go from
a flat number back to hardware: `pic_index = (n - 8) / 32`, `bit = (n - 8) % 32`.

### 2.4 Actual device IRQ assignments

| Device | Flat IRQ | Source | PIC | Bit | CPU IPL |
|---|---|---|---|---|---|
| virt-ctrl | 8 | `hw/m68k/virt.c:79` `PIC_IRQ(1,1)` | #1 | 0 | 1 |
| goldfish-tty | 39 | `hw/m68k/virt.c:75` `PIC_IRQ(1,32)` | #1 | 31 | 1 |
| virtio slot 0 | 40 | `hw/m68k/virt.c:88` `PIC_IRQ(2,1)` | #2 | 0 | 2 |
| virtio slot *i* | 40 + *i* | `hw/m68k/virt.c:215` | see below | | 2–5 |
| virtio slot 127 | 167 | | #5 | 31 | 5 |
| goldfish-rtc 0 | 168 | `hw/m68k/virt.c:70` `PIC_IRQ(6,1)` | #6 | 0 | 6 |
| goldfish-rtc 1 | 169 | `hw/m68k/virt.c:191` (`+ i`) | #6 | 1 | 6 |

**virtio slot → interrupt**, derived from `hw/m68k/virt.c:215` (`PIC_GPIO(40 + i)`):

```
pic_index = 1 + (i / 32)          /* pic_dev[] index: 1..4, i.e. PIC #2..#5   */
bit       = i % 32
cpu_ipl   = 2 + (i / 32)          /* 2..5                                     */
vector    = 26 + (i / 32)         /* 26..29                                   */
```

So slots 0–31 → IPL 2, 32–63 → IPL 3, 64–95 → IPL 4, 96–127 → IPL 5. A guest
that only ever uses the first few slots only needs an IPL 2 handler.

### 2.5 goldfish-pic registers

Offsets from the PIC base (`hw/intc/goldfish_pic.c:22-28`):

| Offset | Name | Access | Semantics |
|---|---|---|---|
| `0x00` | `STATUS` | R | **Population count** of `pending & enabled` (0–32), *not* a mask — `hw/intc/goldfish_pic.c:80-83` |
| `0x04` | `IRQ_PENDING` | R | Bitmask of `pending & enabled` — `:84-87` |
| `0x08` | `IRQ_DISABLE_ALL` | W | Any value: clears both `enabled` and `pending` — `:108-111` |
| `0x0c` | `DISABLE` | W | `enabled &= ~value` — `:112-114` |
| `0x10` | `ENABLE` | W | `enabled |= value` — `:115-117` |

Behavioural facts that matter for the guest handler:

- **The PIC is level-driven and has no "acknowledge" register.** `pending` is
  set/cleared only by the device raising/lowering its line
  (`hw/intc/goldfish_pic.c:58-71`). The handler must quiesce the *device*
  (e.g. write `RTC_CLEAR_INTERRUPT`), not the PIC, or the level stays asserted
  and the CPU re-enters immediately.
- **Reads of `IRQ_PENDING` are masked by `enabled`.** A pending-but-disabled
  line is invisible.
- Any unlisted offset logs `LOG_UNIMP` and reads as 0 / ignores writes
  (`:88-92`, `:118-122`).
- Reset clears `pending` and `enabled` (`:136-148`), so **every line starts
  masked** — the guest must write `ENABLE` before anything is delivered.

### 2.6 Access rules for PIC / TTY / virt-ctrl

`goldfish_pic_ops` (`hw/intc/goldfish_pic.c:127-134`),
`goldfish_tty_ops` (`hw/char/goldfish_tty.c:159-166`) and
`virt_ctrl_ops` (`hw/misc/virt_ctrl.c:82-88`) are all
**`DEVICE_NATIVE_ENDIAN`**, which for an m68k target means big-endian: no byte
swapping is applied. PIC and TTY additionally set
`impl.min_access_size = impl.max_access_size = 4`, so **use 32-bit
(`move.l`) accesses**. Sub-word accesses go through QEMU's access-size
adaptation rather than reaching the device as written, which on a big-endian
target places a byte write at offset 0 into bits 31:24 — a source of
silent, plausible-looking wrong values. Always use longword accesses.

---

## 3. Devices

### 3.1 goldfish-tty — the Phase 1 debug channel

Base `0xff008000`. Registers (`hw/char/goldfish_tty.c:26-34`):

| Offset | Name | Access | Semantics |
|---|---|---|---|
| `0x00` | `PUT_CHAR` | W | Writes low byte to the chardev immediately — `:135-138` |
| `0x04` | `BYTES_READY` | R | Bytes in the RX FIFO — `:52-54` |
| `0x08` | `CMD` | W | See command table — `:139-141` |
| `0x10` | `DATA_PTR` | W | Low 32 bits of DMA address — `:142-144` |
| `0x14` | `DATA_LEN` | W | DMA length — `:148-150` |
| `0x18` | `DATA_PTR_HIGH` | W | High 32 bits of DMA address — `:145-147` |
| `0x20` | `VERSION` | R | `1` (`GOLDFISH_TTY_VERSION`, `:22`, `:55-57`) |

Commands written to `CMD` (`hw/char/goldfish_tty.c:38-43`):

| Value | Command | Effect |
|---|---|---|
| `0` | `INT_DISABLE` | `:78-85` |
| `1` | `INT_ENABLE` | `:86-93` — raises IRQ immediately if FIFO non-empty |
| `2` | `WRITE_BUFFER` | DMA `DATA_LEN` bytes from `DATA_PTR` to host — `:94-107` |
| `3` | `READ_BUFFER` | DMA up to `DATA_LEN` bytes from RX FIFO to `DATA_PTR` — `:108-122` |

**For Phase 1 the only register needed is `PUT_CHAR`.** A 32-bit write of the
character value to `0xff008000` emits one byte. No initialisation, no baud
rate, no ready-polling — `qemu_chr_fe_write_all` is called synchronously and
blocks until written (`:137`). This is the simplest possible bring-up channel
and is why Phase 1 gates on it.

Reset state: RX FIFO empty, interrupts **disabled**, `data_ptr`/`data_len` zero
(`:193-203`). TX therefore works with zero setup; RX needs `CMD_INT_ENABLE` or
polling of `BYTES_READY`.

The chardev is `serial_hd(0)` (`hw/m68k/virt.c:199`), i.e. whatever
`-serial`/`-nographic` selects.

### 3.2 goldfish-rtc — wall clock **and** the only timer

Bases `0xff006000` and `0xff007000`. Registers
(`hw/rtc/goldfish_rtc.c:37-44`):

| Offset | Name | Access | Semantics |
|---|---|---|---|
| `0x00` | `TIME_LOW` | R/W | Low 32 bits of a **nanosecond** counter — `:106-110`, `:144-148` |
| `0x04` | `TIME_HIGH` | R/W | High 32 bits, **latched by the `TIME_LOW` read** — `:107-113` |
| `0x08` | `ALARM_LOW` | R/W | Writing arms the alarm — `:154-157` |
| `0x0c` | `ALARM_HIGH` | R/W | Writing does *not* arm — `:158-160` |
| `0x10` | `IRQ_ENABLED` | R/W | Bit 0 only — `:161-164` |
| `0x14` | `CLEAR_ALARM` | W | Cancels a pending alarm — `:165-167` |
| `0x18` | `ALARM_STATUS` | R | 1 while armed — `:123-125` |
| `0x1c` | `CLEAR_INTERRUPT` | W | Deasserts the IRQ line — `:168-171` |

Facts that shape the Phase 2 design:

- **The counter is nanoseconds since the Unix epoch**, not seconds and not
  ticks: `tick_offset = mktimegm(&tm) * NANOSECONDS_PER_SECOND` adjusted by the
  host clock (`hw/rtc/goldfish_rtc.c:265-268`), and reads add
  `qemu_clock_get_ns(rtc_clock)` (`:60-63`).
- **`TIME_HIGH` must be read second.** `TIME_LOW` latches the upper half into
  `s->time_high` as a side effect (`:108`); reading `TIME_HIGH` first returns a
  stale value. The documented order (`:97-104`) is `TIME_LOW` then `TIME_HIGH`.
- **The alarm is one-shot, not periodic.** `goldfish_rtc_interrupt` clears
  `alarm_running` and sets `irq_pending` (`:51-58`); nothing re-arms it. A
  200 Hz tick must be produced by the guest re-arming the alarm to
  `now + 5_000_000 ns` inside every handler. There is no free-running periodic
  timer anywhere on this machine.
- **Write order to arm:** `ALARM_HIGH` first, then `ALARM_LOW` — only the
  `ALARM_LOW` write calls `goldfish_rtc_set_alarm` (`:156`). Writing `ALARM_LOW`
  with an already-past value fires the interrupt synchronously from within the
  write (`:76-79`), which is the correct behaviour for a re-arm that overran but
  means the handler must be re-entrant-safe or re-arm last.
- **The IRQ is level-held.** `goldfish_rtc_update` asserts while
  `irq_pending & irq_enabled` (`:46-49`). The handler *must* write
  `CLEAR_INTERRUPT`, and `IRQ_ENABLED` must be set to 1 or nothing is ever
  delivered (reset leaves it 0 — `:239-248`).
- **Endianness: big-endian on this machine.** The device has two register-op
  tables selected by a property (`hw/rtc/goldfish_rtc.c:202-221`), and `virt.c`
  sets `big-endian = true` (`hw/m68k/virt.c:187`). So RTC register values need
  **no** byte swap from m68k — unlike virtio-mmio (§5).
- Access size is fixed at 4 (`:207-210`, `:217-220`).

### 3.3 virt-ctrl — reset / shutdown

Base `0xff009000` (`hw/m68k/virt.c:78`). Registers
(`hw/misc/virt_ctrl.c:16-19`):

| Offset | Name | Access | Semantics |
|---|---|---|---|
| `0x00` | `FEATURES` | R | Always `1` (`FEAT_POWER_CTRL`) — `:21`, `:36-38` |
| `0x04` | `CMD` | W | `0`=noop, `1`=reset, `2`=halt, `3`=panic — `:23-28`, `:58-73` |

`CMD_HALT` (2) requests a clean guest shutdown, exiting QEMU
(`hw/misc/virt_ctrl.c:67`). **This is how the Phase 9 launcher should let GEM
"shut down" the emulator.** `DEVICE_NATIVE_ENDIAN`, no swap
(`:85`).

### 3.4 virtio-mmio

128 proxies, `0xff010000 + i*0x200`. Register offsets are the standard ones
(`include/standard-headers/linux/virtio_mmio.h`):

| Offset | Register | Line |
|---|---|---|
| `0x000` | `MAGIC_VALUE` | `:43` |
| `0x004` | `VERSION` | `:46` |
| `0x008` | `DEVICE_ID` | `:49` |
| `0x00c` | `VENDOR_ID` | `:52` |
| `0x010` | `DEVICE_FEATURES` | `:56` |
| `0x014` | `DEVICE_FEATURES_SEL` | `:59` |
| `0x020` | `DRIVER_FEATURES` | `:63` |
| `0x024` | `DRIVER_FEATURES_SEL` | `:66` |
| `0x030` | `QUEUE_SEL` | `:78` |
| `0x034` | `QUEUE_NUM_MAX` | `:81` |
| `0x038` | `QUEUE_NUM` | `:84` |
| `0x044` | `QUEUE_READY` | `:99` |
| `0x050` | `QUEUE_NOTIFY` | `:102` |
| `0x060` | `INTERRUPT_STATUS` | `:105` |
| `0x064` | `INTERRUPT_ACK` | `:108` |
| `0x070` | `STATUS` | `:111` |
| `0x080` / `0x084` | `QUEUE_DESC_LOW` / `_HIGH` | `:114-115` |
| `0x090` / `0x094` | `QUEUE_AVAIL_LOW` / `_HIGH` | `:118-119` |
| `0x0a0` / `0x0a4` | `QUEUE_USED_LOW` / `_HIGH` | `:122-123` |
| `0x0fc` | `CONFIG_GENERATION` | `:137` |
| `0x100` | `CONFIG` (device-specific) | `:141` |

Legacy-only registers `GUEST_PAGE_SIZE` (`0x028`), `QUEUE_ALIGN` (`0x03c`) and
`QUEUE_PFN` (`0x040`) exist but are rejected with a `LOG_GUEST_ERROR` in modern
mode (`hw/virtio/virtio-mmio.c:333-340`, `:363-372`, `:373-387`).

Constants (`include/hw/virtio/virtio-mmio.h:39-42`):

| | Value |
|---|---|
| `MAGIC_VALUE` | `0x74726976` (`"virt"` little-endian) |
| `VERSION` (modern) | `2` |
| `VERSION` (legacy) | `1` |
| `VENDOR_ID` | `0x554D4551` (`"QEMU"` little-endian) |

**This machine is modern (v2) virtio.** `virt.c` sets `force-legacy = false`
per proxy (`hw/m68k/virt.c:212`), overriding the property default of `true`
(`hw/virtio/virtio-mmio.c:767`). That single line drives §5.

**Empty slots are safe to probe.** With no backend attached, reads return the
real magic, version and vendor ID but `DEVICE_ID = 0`, and all other registers
read as 0 / ignore writes (`hw/virtio/virtio-mmio.c:92-115`, `:256-262`).
Enumeration is therefore: walk all 128 slots, check `MAGIC_VALUE`, skip any
slot whose `DEVICE_ID` is 0.

Other behaviours worth knowing before Phase 3:

- **Non-config registers reject non-32-bit access** with `LOG_GUEST_ERROR`,
  returning 0 on read and dropping the write
  (`hw/virtio/virtio-mmio.c:143-148`, `:298-303`). The **config space** at
  `0x100+` *does* accept 1/2/4-byte access (`:117-142`, `:264-297`).
- **`QUEUE_NUM_MAX` returns `VIRTQUEUE_MAX_SIZE` for any queue the device has,
  and 0 for one it doesn't** (`:174-178`). Use it to discover how many
  virtqueues exist.
- **Feature negotiation is two 32-bit halves.** Write 0 or 1 to
  `DEVICE_FEATURES_SEL` / `DRIVER_FEATURES_SEL` to select the low or high word
  (`:305-311`, `:326-332`). In modern mode `DRIVER_FEATURES` writes are latched
  into `proxy->guest_features[sel]` and are **not applied until the driver sets
  `FEATURES_OK` in `STATUS`** (`:323-324`, `:431-435`). Only `sel` values 0 and
  1 exist; anything non-zero is treated as 1.
- **Queue setup order is enforced.** `QUEUE_READY = 1` is what actually commits
  the descriptor / avail / used addresses and the queue size
  (`:396-409`). All of `QUEUE_NUM`, `QUEUE_DESC_*`, `QUEUE_AVAIL_*`,
  `QUEUE_USED_*` must be written **before** `QUEUE_READY`, and each applies to
  whichever queue `QUEUE_SEL` currently names.
- **`INTERRUPT_STATUS` bits:** `1` = used-ring progressed, `2` = config changed
  (`include/standard-headers/linux/virtio_mmio.h:149-150`). Acknowledge by
  writing the same bits to `INTERRUPT_ACK`, which clears them from `isr` and
  re-evaluates the IRQ line (`hw/virtio/virtio-mmio.c:422-425`). The line is
  level-driven off `isr != 0` (`:539-551`) — **failing to ack leaves the m68k
  IPL asserted forever.**
- **Writing `STATUS = 0` triggers a full soft reset** of the device and clears
  every queue's `enabled` flag (`:443-445`, `:72-83`).

---

## 4. Boot protocol

### 4.1 How the kernel is loaded

Only the `-kernel` path exists; there is no ROM, no BIOS and no bootloader.
`hw/m68k/virt.c:231-237`:

```c
kernel_size = load_elf(kernel_filename, NULL, NULL, NULL,
                       &elf_entry, NULL, &high, NULL, ELFDATA2MSB,
                       EM_68K, 0, 0);
```

Requirements this imposes on the Shinogi EmuTOS artifact:

- It must be an **ELF** file — `ELFDATA2MSB` (big-endian) and `EM_68K`.
  Anything else is a fatal "could not load kernel".
- Segments are placed at their **physical** addresses (`p_paddr`), so the link
  script controls exactly where the image lands.
- No `-bios`, no `-drive if=pflash`. The image *must* arrive via `-kernel`.

### 4.2 Reset PC and initial stack

```c
static void main_cpu_reset(void *opaque)
{
    ResetInfo *reset_info = opaque;
    ...
    cpu_reset(cs);
    cpu->env.aregs[7] = reset_info->initial_stack;
    cpu->env.pc = reset_info->initial_pc;
}
```
— `hw/m68k/virt.c:96-105`, registered at `:151`

- **`initial_pc` = the ELF entry point** (`e_entry`), assigned at
  `hw/m68k/virt.c:238`.
- **`initial_stack` is never assigned.** `reset_info` comes from `g_new0`
  (`hw/m68k/virt.c:145`) and no code writes `initial_stack`. Therefore
  **A7 = 0 on entry.** The EmuTOS entry point must load its own stack pointer
  as its first instruction; any `bsr`/`jsr` before that writes over the
  exception vector table at address 0.
- The classic 68k "fetch SP and PC from `0x000`/`0x004`" reset behaviour does
  **not** apply — `cpu_reset()` runs first but PC and A7 are then overwritten
  unconditionally.

### 4.3 CPU state at entry

From `m68k_cpu_reset_hold` (`target/m68k/cpu.c:99-116`):

- `memset(env, 0, ...)` — so **VBR = 0**, all data/address registers 0, MMU
  translation control 0 (MMU off).
- `cpu_m68k_set_sr(env, SR_S | SR_I)` — **supervisor mode, interrupt mask 7**
  (all maskable interrupts blocked). The guest must lower the IPL mask before
  anything from §2 is delivered.
- Default CPU is **68040** (`mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68040")`,
  `hw/m68k/virt.c:318`), overridable with `-cpu`.

Because VBR is 0 and RAM starts at 0, the m68k vector table occupies
`0x000`–`0x3FF` of RAM. The EmuTOS link script must either place the image
above `0x400` and build the vector table at runtime, or set VBR to a table of
its own early in the entry path.

### 4.4 Where the bootinfo lands — `parameters_base`

```c
reset_info->initial_pc = elf_entry;
parameters_base = (high + 1) & ~1;
```
— `hw/m68k/virt.c:238-239`

`high` is the value `load_elf` reports as the **highest loaded address**, and
`include/hw/elf_ops.h.inc:578-579` computes it as:

```c
if ((addr + mem_size) > high)
    high = addr + mem_size;
```

i.e. `max(p_paddr + p_memsz)` over all loaded segments — **one past the last
byte, and it includes BSS** (`p_memsz`, not `p_filesz`). So:

> **`parameters_base` = the end of the EmuTOS image including BSS, rounded up
> to a 2-byte boundary.**

**Nothing tells the guest this address at runtime** — no register holds it, and
it is not passed in any CPU register (contrast: `A7` and `PC` are the only
state set). The guest is expected to know it, exactly as Linux/m68k does, by
taking the address of a symbol at the very end of its own image. The Shinogi
link script must therefore export an `_end`-style symbol placed after BSS, and
the bootinfo parser must start at `ALIGN(_end, 2)`.

The blob is installed as a fixed ROM region:

```c
rom_add_blob_fixed_as("bootinfo", param_blob, param_ptr - param_blob,
                      parameters_base, cs->as);
```
— `hw/m68k/virt.c:303-304`

Being a ROM blob means QEMU rewrites it into RAM on **every machine reset**, not
just at startup. The guest may safely reuse that memory after parsing, but a
warm reset restores it.

### 4.5 Bootinfo record format

```c
struct bi_record {
    uint16_t tag;     /* tag ID */
    uint16_t size;    /* size of record (in bytes) */
    uint32_t data[0]; /* data */
};
```
— `include/standard-headers/asm-m68k/bootinfo.h:29-33`

All fields are **big-endian** — the emitting macros use `stw_be_p` / `stl_be_p`
(`hw/m68k/bootinfo.h:15-43`). `size` is the size of the **whole record**
including the 4-byte header, so iteration is `p += rec->size`, and the list ends
at a `BI_LAST` record. Padding rules:

- `BOOTINFO0` — header only, `size = 4` (`hw/m68k/bootinfo.h:15-21`)
- `BOOTINFO1` — one `uint32_t`, `size = 8` (`:23-31`)
- `BOOTINFO2` — two `uint32_t`, `size = 12` (`:33-43`)
- `BOOTINFOSTR` — NUL-terminated string, `size` rounded **up to a multiple of
  4** (`:45-58`)
- `BOOTINFODATA` — big-endian `uint16_t` length then raw bytes, `size` rounded
  up to a multiple of 4 (`:60-74`)

A parser that trusts `rec->size` handles all five uniformly. **A parser that
assumes fixed sizes per tag will desynchronise** on `BI_COMMAND_LINE` and
`BI_RNG_SEED`.

### 4.6 The exact tag sequence this machine emits

In emission order (`hw/m68k/virt.c:242-302`):

| # | Tag | Value | Payload | Citation |
|---|---|---|---|---|
| 1 | `BI_MACHTYPE` | `0x0001` | `MACH_VIRT` = **14** | `:242`; values at `bootinfo.h:50`, `:85` |
| 2 | `BI_CPUTYPE` (+`BI_MMUTYPE`/`BI_FPUTYPE`) | `0x0002`/`0x0004`/`0x0003` | depends on `-cpu` | `:243-256` |
| 3 | `BI_MEMCHUNK` | `0x0005` | base `0`, size `ram_size` | `:257` |
| 4 | `BI_VIRT_QEMU_VERSION` | `0x8000` | `(maj<<24)｜(min<<16)｜(micro<<8)` | `:259-261`; tag at `bootinfo-virt.h:9` |
| 5 | `BI_VIRT_GF_PIC_BASE` | `0x8001` | `0xff000000`, `1` | `:262-263`; `bootinfo-virt.h:10` |
| 6 | `BI_VIRT_GF_RTC_BASE` | `0x8002` | `0xff006000`, `168` | `:264-265`; `bootinfo-virt.h:11` |
| 7 | `BI_VIRT_GF_TTY_BASE` | `0x8003` | `0xff008000`, `39` | `:266-267`; `bootinfo-virt.h:12` |
| 8 | `BI_VIRT_CTRL_BASE` | `0x8005` | `0xff009000`, `8` | `:268-269`; `bootinfo-virt.h:14` |
| 9 | `BI_VIRT_VIRTIO_BASE` | `0x8004` | `0xff010000`, `40` | `:270-271`; `bootinfo-virt.h:13` |
| 10 | `BI_COMMAND_LINE` | `0x0007` | string, only if `-append` given | `:273-276` |
| 11 | `BI_RNG_SEED` | `0x0008` | 32 random bytes | `:279-282` |
| 12 | `BI_RAMDISK` | `0x0006` | base, size — only if `-initrd` given | `:296-297` |
| 13 | `BI_LAST` | `0x0000` | — | `:302` |

Note the emission order puts `BI_VIRT_CTRL_BASE` (`0x8005`) *before*
`BI_VIRT_VIRTIO_BASE` (`0x8004`) — **tags are not sorted**; parse by walking,
not by seeking.

The second word of each `BI_VIRT_*_BASE` record is the **flat IRQ number** from
§2.3, not a CPU level. `BI_VIRT_GF_PIC_BASE`'s second word is `1`
(`VIRT_GF_PIC_IRQ_BASE`, `hw/m68k/virt.c:65`) — the first *PIC number*, not a
flat IRQ. That inconsistency is in QEMU; do not "fix" it in the parser.

`VIRT_BOOTI_VERSION` is `MK_BI_VERSION(2, 0)` (`bootinfo-virt.h:19`), but note
that `virt.c` **never emits a `struct bootversion`** and never checks one —
that structure (`bootinfo.h:162-169`) is a bootstrap-side convention for real
Linux boot loaders. Shinogi does not need it.

### 4.7 Practical consequence for EmuTOS

EmuTOS could hardcode every address in §1, since they are compile-time
constants in QEMU and stable across machine versions (§6). The recommendation
is nevertheless to **parse the bootinfo for `BI_MEMCHUNK` at minimum** — RAM
size is the one value genuinely set by the user at runtime (`-m`), and a
hardcoded 128 MiB would silently misbehave under any other `-m`.

---

## 5. Endianness — the Phase 3 trap, and it is worse than expected

The brief anticipated that virtio's in-memory structures are little-endian
while m68k is big-endian. That is true, and there is a **second, independent**
swap that is easy to miss.

### 5.1 MMIO registers are byte-swapped too

Modern virtio-mmio's `MemoryRegionOps` declares little-endian registers:

```c
static const MemoryRegionOps virtio_legacy_mem_ops = {
    ...
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps virtio_mem_ops = {
    ...
    .endianness = DEVICE_LITTLE_ENDIAN,
};
```
— `hw/virtio/virtio-mmio.c:527-537`

and `virtio_mmio_realizefn` picks `virtio_mem_ops` whenever `legacy` is false
(`hw/virtio/virtio-mmio.c:789-793`) — which, per `hw/m68k/virt.c:212`, is
always on this machine.

QEMU's memory core then swaps on every access whose endianness differs from the
region's:

```c
static void adjust_endianness(MemoryRegion *mr, uint64_t *data, MemOp op)
{
    if ((op & MO_BSWAP) != devend_memop(mr->ops->endianness)) {
        switch (op & MO_SIZE) {
        ...
        case MO_32:
            *data = bswap32(*data);
```
— `system/memory.c:359-372`, called from `:1486` (read) and `:1534` (write)

For an m68k guest (`op` carries `MO_BE`) against a `DEVICE_LITTLE_ENDIAN`
region, the condition holds and **every 32-bit register value is byte-swapped
in both directions**.

**Consequences:**

- A `move.l` of `0x00000001` to `STATUS` (`0x070`) arrives at the device as
  `0x01000000`. The guest must write `bswap32(value)`.
- `MAGIC_VALUE` reads back as **`0x76697274`** on the guest side, not
  `0x74726976`. That is the ASCII `"virt"` in guest-readable order — a
  convenient sanity check that the swap is being applied consistently. Reading
  `0x74726976` means a swap is missing somewhere.
- Same for `VENDOR_ID`: the guest sees `0x51454D55` (`"QEMU"`), not
  `0x554D4551`.
- This applies to *every* virtio-mmio register including `QUEUE_NOTIFY`,
  `INTERRUPT_ACK` and the device config space at `0x100+`.
- It applies **only** to virtio-mmio. The PIC, TTY and virt-ctrl are
  `DEVICE_NATIVE_ENDIAN` (§2.6) and the RTC is explicitly big-endian
  (§3.2) — those need **no** swap. Mixing the two up is the single most likely
  Phase 2/3 bug.

### 5.2 In-memory structures are little-endian

Descriptor tables, available rings and used rings live in guest RAM and are
read by QEMU through the virtio core, which for a modern device treats them as
little-endian regardless of target. These are *not* touched by
`adjust_endianness` — they are DMA, not MMIO — so the guest must byte-swap them
itself when composing and interpreting them.

### 5.3 The rule for Phase 3

Write the swap helpers **first**, before any device code, and route every
virtio access through them:

```
vio_read32(slot, reg)   -> bswap32(*(volatile uint32_t *)(slot + reg))
vio_write32(slot, reg, v) -> *(volatile uint32_t *)(slot + reg) = bswap32(v)
le32(x) / le16(x)       -> for descriptor / ring fields in RAM
```

Never use a native-endian accessor against a virtio structure or register. The
failure mode of getting this wrong is not a clean fault — it is a queue that
appears to be set up correctly and then silently transfers garbage.

---

## 6. Machine versioning

`virt` is an alias for the newest versioned machine, currently `virt-11.0`
(`hw/m68k/virt.c:365-373`). Versions back to `virt-6.0` are defined
(`:370-478`), and all of them share the same `virt_init` — the version options
functions add only generic `hw_compat_*` property lists (`:375-478`), none of
which touch the m68k devices.

**Therefore every address, IRQ number and register layout in this document is
identical across `virt-6.0` … `virt-11.0`.** The Phase 9 launcher can safely
pass `-M virt` without pinning a version, though pinning is still the safer
choice for reproducibility.

---

## 7. Summary of constants for the guest port

```c
/* Memory map — hw/m68k/virt.c:64-88 */
#define VIRT_RAM_BASE        0x00000000UL
#define VIRT_GF_PIC_BASE     0xff000000UL   /* x6, stride 0x1000 */
#define VIRT_GF_PIC_NB       6
#define VIRT_GF_RTC_BASE     0xff006000UL   /* x2, stride 0x1000 */
#define VIRT_GF_RTC_NB       2
#define VIRT_GF_TTY_BASE     0xff008000UL
#define VIRT_CTRL_BASE       0xff009000UL
#define VIRT_VIRTIO_BASE     0xff010000UL   /* x128, stride 0x200 */
#define VIRT_VIRTIO_NB       128
#define VIRT_VIRTIO_STRIDE   0x200

/* Flat IRQ numbers — hw/m68k/virt.c:59-88 */
#define VIRT_IRQ_CTRL        8              /* PIC #1 bit 0  -> IPL 1 */
#define VIRT_IRQ_TTY         39             /* PIC #1 bit 31 -> IPL 1 */
#define VIRT_IRQ_VIRTIO_BASE 40             /* PIC #2..#5    -> IPL 2..5 */
#define VIRT_IRQ_RTC_BASE    168            /* PIC #6 bit 0  -> IPL 6 */

/* pic index / bit / level for a flat IRQ n — hw/m68k/virt.c:61-62,
 * hw/intc/m68k_irqc.c:48-54, include/hw/intc/m68k_irqc.h:19 */
#define VIRT_PIC_INDEX(n)    (((n) - 8) / 32)          /* 0..5           */
#define VIRT_PIC_BIT(n)      (((n) - 8) % 32)
#define VIRT_PIC_ADDR(n)     (VIRT_GF_PIC_BASE + VIRT_PIC_INDEX(n) * 0x1000)
#define VIRT_IPL(n)          (VIRT_PIC_INDEX(n) + 1)   /* 1..6           */
#define VIRT_VECTOR(n)       (VIRT_PIC_INDEX(n) + 25)  /* 25..30         */

/* goldfish-pic registers — hw/intc/goldfish_pic.c:22-28 */
#define GF_PIC_STATUS        0x00   /* R: popcount(pending & enabled) */
#define GF_PIC_IRQ_PENDING   0x04   /* R: mask                        */
#define GF_PIC_DISABLE_ALL   0x08
#define GF_PIC_DISABLE       0x0c
#define GF_PIC_ENABLE        0x10

/* goldfish-tty registers — hw/char/goldfish_tty.c:26-43 */
#define GF_TTY_PUT_CHAR      0x00
#define GF_TTY_BYTES_READY   0x04
#define GF_TTY_CMD           0x08
#define GF_TTY_DATA_PTR      0x10
#define GF_TTY_DATA_LEN      0x14
#define GF_TTY_DATA_PTR_HIGH 0x18
#define GF_TTY_VERSION       0x20
#define GF_TTY_CMD_INT_DISABLE  0
#define GF_TTY_CMD_INT_ENABLE   1
#define GF_TTY_CMD_WRITE_BUFFER 2
#define GF_TTY_CMD_READ_BUFFER  3

/* goldfish-rtc registers — hw/rtc/goldfish_rtc.c:37-44 (BIG-ENDIAN here) */
#define GF_RTC_TIME_LOW        0x00
#define GF_RTC_TIME_HIGH       0x04
#define GF_RTC_ALARM_LOW       0x08
#define GF_RTC_ALARM_HIGH      0x0c
#define GF_RTC_IRQ_ENABLED     0x10
#define GF_RTC_CLEAR_ALARM     0x14
#define GF_RTC_ALARM_STATUS    0x18
#define GF_RTC_CLEAR_INTERRUPT 0x1c

/* virt-ctrl — hw/misc/virt_ctrl.c:16-28 */
#define VIRT_CTRL_FEATURES   0x00
#define VIRT_CTRL_CMD        0x04
#define VIRT_CTRL_CMD_NOOP   0
#define VIRT_CTRL_CMD_RESET  1
#define VIRT_CTRL_CMD_HALT   2
#define VIRT_CTRL_CMD_PANIC  3

/* virtio-mmio — include/hw/virtio/virtio-mmio.h:39-42.
 * NOTE: registers are LITTLE-ENDIAN; the guest sees these byte-swapped. */
#define VIRTIO_MMIO_MAGIC_HOST   0x74726976UL  /* as QEMU stores it   */
#define VIRTIO_MMIO_MAGIC_GUEST  0x76697274UL  /* as m68k reads it    */
#define VIRTIO_MMIO_VENDOR_GUEST 0x51454D55UL
#define VIRTIO_MMIO_VERSION_V2   2
```

---

## 8. Findings that change the plan

1. **A7 = 0 at reset** (§4.2). The EmuTOS entry point must establish its own
   stack as its first action. Not a Phase 1 blocker, but a Phase 1 crash if
   missed.
2. **virtio-mmio *registers* are byte-swapped, not just the rings** (§5.1).
   This is an addition to the brief's Phase 3 warning and doubles the surface
   area of the endianness discipline.
3. **There is no periodic timer on this machine** (§3.2). The 200 Hz tick in
   Phase 2 must be built from the goldfish-rtc one-shot alarm, re-armed inside
   every handler. Expect tick drift unless the re-arm is computed from the
   previous deadline rather than from "now".
4. **`parameters_base` is not communicated at runtime** (§4.4). It is
   `ALIGN(end-of-image-including-BSS, 2)`, so the link script must export a
   symbol there. Getting this wrong yields a parser walking random memory.
5. **The PIC has no acknowledge register** (§2.5). Interrupt handlers must
   silence the source device, and every line starts masked.
6. **Address constants are stable across all `virt-*` machine versions** (§6),
   so hardcoding is safe; only RAM size genuinely varies at runtime.

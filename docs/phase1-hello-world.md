# Phase 1 — Hello World

**Gate: characters appear on the host terminal. Met.**

EmuTOS builds as a big-endian m68k ELF, QEMU loads it with `-kernel`, and
`kprintf` output reaches the host through the goldfish-tty.

## Reproducing

Toolchain: `/opt/cross-mintelf/bin` (`m68k-atari-mintelf-gcc` 15.2.0,
MiNT ELF 20250810), already on `PATH`.

```sh
cd ~/git/emutos                 # branch: shinogi
make clean
make ELF=1 TOOLCHAIN_PREFIX=m68k-atari-mintelf- \
     QEMU_VIRT_DEFS=-DENABLE_KDEBUG qemu-virt

qemu-system-m68k -M virt -m 128 -kernel emutos-virt.elf -nographic
```

`make clean` is required when switching targets — EmuTOS regenerates the
desktop resource headers per target, and stale objects from a previous
target fail to compile.

`ENABLE_KDEBUG` is needed to see anything. It is a per-file `#define`
inside EmuTOS rather than a build switch, so a stock build contains no
`kprintf` calls in the boot path at all and is silent. `QEMU_VIRT_DEFS`
exists on the `qemu-virt` target to inject it.

## Observed output

```
bios_init()
processor_init()
Address Bus width is 32-bit
vecs_init()
init_delay()
init_delay loopcount_1_msec=3800
machine_detect()
ttram_detect()
ttram_detect(): ramtop=0x00000000
detected_busses = 0x0
machine_init()
bmem_init()
Memory map before balloc() adjustments:
        _text = 0x00002140
       _etext = 0x0004eeae
         _bss = 0x0004eeae
        _ebss = 0x0005d36e
       stkbot = 0x00000800
       stktop = 0x00001000
_end_os_stram = 0x0005d370
       membot = 0x0005d370
       memtop = 0x00000000
cookie_init()
...
font_init()
screen_init_mode()
linea_init()
linea_init(): 320x200 4-plane (v_lin_wr=160)
screen_init_address()
before balloc_stram: membot=0x0005d370, memtop=0x00000000
balloc_stram(32768, 1)
```

This is well past the gate. The boot then stalls in `balloc_stram`
because `memtop` is zero — see "Known stopping point" below.

## The build artifact

```
$ file emutos-virt.elf
ELF 32-bit MSB executable, Motorola m68k, 68000, version 1 (SYSV), statically linked

Entry point address: 0x2170          <- _main
  LOAD 0x00000000 filesz 0x00000 memsz 0x02140 RW   sysvars, stack, .low_stram
  LOAD 0x00002140 filesz 0x4843e memsz 0x4843e R E  text + rodata
  LOAD 0x0004eeae filesz 0x00000 memsz 0x0e4c4 RW   bss
```

`ELFDATA2MSB` + `EM_68K` is what `load_elf` demands at
`hw/m68k/virt.c:231-233`. Highest loaded address is `0x5D370`, so
`parameters_base` — where QEMU deposits the bootinfo — is `0x5D370`
(see `docs/qemu-virt-machine.md` §4.4).

## What changed in EmuTOS

Branch `shinogi` of `rmahlert/emutos`, commit `87f1e29f`. Five files,
+108/-4, all machine-conditional:

| File | Change |
|---|---|
| `bios/qemuvirt.h` | new — goldfish-tty register definitions |
| `bios/qemuvirt.c` | new — `qemuvirt_kprintf_outc()` |
| `include/config.h` | `TARGET_QEMU_VIRT` → `EMUTOS_LIVES_IN_RAM`; `MACHINE_QEMU_VIRT` defaults; `QEMU_VIRT_DEBUG_PRINT` |
| `bios/kprint.c` | dispatch arm in `vkprintf()` |
| `bios/processor.S` | skip the 68060 PCR probe |
| `emutos.ld` | `OUTPUT_FORMAT(elf32-m68k)` + `ENTRY(_main)` for this target |
| `Makefile` | `qemu-virt` target, `qemuvirt.c` in `bios_src` |

No shared code was restructured and no addresses were invented — the
existing `stram` region already places sysvars at `0x000`, `.low_stram`
below `0x8000`, and text above, which is exactly where `-kernel` loads
them.

## Findings

**Phase 0's A7 = 0 warning turned out to be harmless here.** `_realmain`
(`bios/startup.S:335`) opens with `move #0x2700,sr` and then goes
straight to `lea _stktop,sp`. The blocks in between are all excluded by
this configuration — no `CONF_WITH_TT_MMU`, not Amiga, and the ST_MMU
block is gated on `!EMUTOS_LIVES_IN_RAM`. Nothing touches the stack
before SP is loaded. Confirmed at the first crash dump, which showed
`A7 = 0x00000f88` — the real `_stktop`.

**QEMU aborts the machine on an unimplemented control register.** The
first boot attempt died with:

```
qemu: fatal: Unimplemented control register read 0x808
PC = 00002382   SR = 2704
```

`0x808` is the 68060 PCR. `processor.S:107` reads it deliberately,
expecting a 68040 to take an exception into the handler installed a few
instructions earlier. QEMU does not raise an exception — it terminates
the VM. Since QEMU is out of scope for modification, the probe is
skipped under `MACHINE_QEMU_VIRT`, which mirrors the existing
`MACHINE_LISA` workaround for `movec`/`pmove` at `bios/startup.S:398`.
The CPU is whatever `-cpu` selected, defaulting to 68040.

This is worth remembering as a class of problem rather than a one-off:
**any EmuTOS probe that relies on catching a fault may abort QEMU
instead of trapping.** Expect more of these as later phases enable more
hardware detection.

**`CONF_SERIAL_CONSOLE` must stay off.** Setting it looked reasonable —
this machine's console *is* serial — but it selects the MFP RS232 port,
which does not exist here, and pulls in `RS232_DEBUG_PRINT`. That
collides with `QEMU_VIRT_DEBUG_PRINT` and trips EmuTOS's
"only one debug print backend" assertion.

## Known stopping point

The boot stalls in `balloc_stram` with `memtop = 0x00000000`.
`machine_detect` finds no memory because there is no Atari memory
controller to probe, and nothing yet tells EmuTOS how much RAM exists.

The answer is `BI_MEMCHUNK` in the bootinfo at `parameters_base`
(`docs/qemu-virt-machine.md` §4.6), which carries base `0` and the real
`ram_size` from `-m`. That is the first task of Phase 2 and is tracked
separately — it is deliberately **not** fixed here, since the Phase 1
gate is "characters appear on the host terminal. Nothing else."

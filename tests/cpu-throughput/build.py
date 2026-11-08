#!/usr/bin/env python3
"""Build identical bare-metal workloads for QEMU and a WinUAE test ROM."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

CASES = {
    "integer": (500_000_000, "", """
        moveq #1,%d1
        moveq #3,%d2
        moveq #5,%d3
1:      addl %d2,%d1
        addql #1,%d2
        subl %d2,%d3
        subql #1,%d7
        bne 1b
        movel %d1,%d6
        eorl %d2,%d6
        eorl %d3,%d6
""", lambda n: ((1+3*n+n*(n-1)//2) & 0xffffffff) ^ ((3+n) & 0xffffffff) ^ ((5-3*n-n*(n+1)//2) & 0xffffffff)),
    "branch": (250_000_000, "", """
        moveq #0,%d1
        moveq #1,%d2
1:      addql #1,%d1
        btst #0,%d1
        beq 2f
        addql #3,%d2
        bra 3f
2:      subql #1,%d2
3:      subql #1,%d7
        bne 1b
        movel %d2,%d6
""", lambda n: 1 + 3*((n+1)//2) - n//2),
    "shift": (250_000_000, "", """
        moveq #0,%d1
        moveq #0,%d3
1:      movel %d1,%d2
        lsll #2,%d2
        lsrl #2,%d2
        addl %d2,%d3
        addql #1,%d1
        subql #1,%d7
        bne 1b
        movel %d3,%d6
""", lambda n: n*(n-1)//2 & 0xffffffff),
    "memory": (1024, """
        lea 0x00400000,%a0
        moveq #0,%d0
1:      movel %d0,(%a0)+
        addql #1,%d0
        cmpil #65536,%d0
        bne 1b
""", """
        moveq #0,%d6
1:      lea 0x00400000,%a0
        lea 0x00500000,%a1
        lea 0x00440000,%a2
2:      movel (%a0)+,%d1
        movel %d1,(%a1)+
        addl %d1,%d6
        cmpal %a2,%a0
        bne 2b
        subql #1,%d7
        bne 1b
""", lambda n: n*65536*65535//2 & 0xffffffff),
    "fpu": (20_000_000, "", """
        fmove.l #1,%fp0
        fmove.l #1,%fp1
1:      fadd.x %fp1,%fp0
        fsub.x %fp1,%fp0
        fmul.x %fp1,%fp0
        fsqrt.x %fp0,%fp0
        subql #1,%d7
        bne 1b
        fmove.l %fp0,%d6
""", lambda n: 1),
}

# Separate RAM-index conflicts from sequential-copy throughput.
count, setup, body, expected = CASES["memory"]
CASES["memory-offset"] = (count, setup, body.replace("0x00500000", "0x00501000"), expected)
CASES["memory-flush"] = (count, setup, body.replace(
    "1:      lea 0x00400000", "1:      .word 0xf518 /* pflusha */\n        lea 0x00400000"), expected)
CASES["memory-copy"] = (4096, setup, """
        moveq #0,%d6
1:      lea 0x00400000,%a0
        lea 0x00500000,%a1
        lea 0x00440000,%a2
2:      movel (%a0)+,(%a1)+
        cmpal %a2,%a0
        bne 2b
        addql #1,%d6
        subql #1,%d7
        bne 1b
""", lambda n: n)
CASES["memory-movem"] = (4096, setup, """
        moveq #0,%d6
1:      lea 0x00400000,%a0
        lea 0x00500000,%a1
        lea 0x00440000,%a2
2:      moveml (%a0)+,%d0-%d5/%a3-%a4
        moveml %d0-%d5/%a3-%a4,(%a1)
        lea 32(%a1),%a1
        cmpal %a2,%a0
        bne 2b
        addql #1,%d6
        subql #1,%d7
        bne 1b
""", lambda n: n)


def command(*args):
    subprocess.run([str(x) for x in args], check=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--bootstrap-cpu", choices=("m68040", "m68060"), default="m68060")
    p.add_argument("--cross", default="m68k-atari-mintelf-")
    p.add_argument("--scale", type=float, default=1)
    a = p.parse_args()
    if not 0 < a.scale <= 4:
        p.error("scale must be positive and at most 4")
    a.out.mkdir(parents=True, exist_ok=True)
    manifest = {}
    for name, (default, setup, body, expected) in CASES.items():
        n = max(2, int(default*a.scale)//2*2)
        if name == "shift" and n >= 1 << 30:
            p.error("shift count must fit its 30-bit data range")
        warm = min(n, max(2, n//128))
        prefix = a.out/name
        output_check = output_clear = output_helper = ""
        if name.startswith("memory"):
            destination = 0x501000 if name == "memory-offset" else 0x500000
            output_clear = "        bsr clear_output\n"
            output_check = f"""
        lea {destination},%a0
        lea {destination+0x40000},%a1
        moveq #0,%d2
1:      movel (%a0)+,%d3
        addl %d3,%d2
        cmpal %a1,%a0
        bne 1b
        cmpil #0x7fff8000,%d2
        bne fail
"""
            output_helper = f"""
clear_output:
        lea {destination},%a0
        lea {destination+0x40000},%a1
1:      clrl (%a0)+
        cmpal %a1,%a0
        bne 1b
        rts
"""
        pcr = "" if a.bootstrap_cpu == "m68040" else """
        .word 0x4e7a,0x0808 /* movec PCR,D0 */
        andil #0xfffffffd,%d0
        .word 0x4e7b,0x0808 /* movec D0,PCR */
"""
        asm = f"""
        .text
        .globl _start
_start:
{pcr}
        movel #0x80008000,%d0
        movec %d0,%cacr
{setup}
        movel #{warm},%d7
        bsr bench
        movel #{expected(warm) & 0xffffffff},%d5
        bsr verify
{output_clear}
        moveq #66,%d0
        jsr (%a6)
        moveq #10,%d0
        jsr (%a6)
        movel #{n},%d7
        bsr bench
        moveq #69,%d0
        jsr (%a6)
        moveq #10,%d0
        jsr (%a6)
        movel #{expected(n) & 0xffffffff},%d5
        bsr verify
        moveq #80,%d0
        jsr (%a6)
        bra finish
verify:
        tstl %d7
        bne fail
        cmpl %d5,%d6
        bne fail
{output_check}
        rts
fail:
        moveq #70,%d0
        jsr (%a6)
finish:
        moveq #10,%d0
        jsr (%a6)
1:      stop #0x2700
        bra 1b
{output_helper}
bench:
{body}
        rts
        .balign 4
"""
        prefix.with_suffix(".S").write_text(asm)
        command(a.cross+"gcc", "-c", "-m68040", "-nostdlib", prefix.with_suffix(".S"), "-o", prefix.with_suffix(".o"))
        command(a.cross+"ld", "-m", "m68kelf", "-Ttext", "0x200000", "-e", "_start", prefix.with_suffix(".o"), "-o", prefix.with_suffix(".payload.elf"))
        command(a.cross+"objcopy", "-O", "binary", "-j", ".text", prefix.with_suffix(".payload.elf"), prefix.with_suffix(".bin"))
        data = prefix.with_suffix(".bin").read_bytes()
        # The workload and its addresses are identical; only boot and UART differ.
        qemu = f"""
        .section .boot,\"ax\"
        .globl qemu_start
qemu_start:
        lea 0x009ff000,%sp
        lea emit(%pc),%a6
        jmp 0x00200000
emit:
        movel %d0,0xff008000
        rts
"""
        boot = prefix.with_suffix(".qemu.S")
        boot.write_text(qemu)
        command(a.cross+"gcc", "-c", "-m68040", boot, "-o", prefix.with_suffix(".qemu.o"))
        script = prefix.with_suffix(".ld")
        script.write_text("ENTRY(qemu_start)\nSECTIONS { . = 0x1000; .boot : { *(.boot) } . = 0x200000; .text : { *(.text) } }\n")
        command(a.cross+"ld", "-m", "m68kelf", "-T", script, prefix.with_suffix(".qemu.o"), prefix.with_suffix(".o"), "-o", prefix.with_suffix(".elf"))
        rom = f"""
        .text
        .long 0x001ff000,reset
reset:
        movew #0x2700,%sr
        movew #0x7fff,0x00dff096
        movew #0x7fff,0x00dff09a
        movew #0x7fff,0x00dff09c
        moveb #0,0x00bfe001
        moveb #0x20,0x00e80048
        lea 0x009ff000,%sp
        movew #30,0x00dff032
        lea payload(%pc),%a0
        lea 0x00200000,%a1
        movel #{len(data)//4},%d0
1:      movel (%a0)+,(%a1)+
        subql #1,%d0
        bne 1b
        lea emit(%pc),%a6
        jmp 0x00200000
emit:
        oriw #0x100,%d0
        movew %d0,0x00dff030
        rts
        .balign 4
payload:
        .incbin \"{prefix.resolve().with_suffix('.bin')}\"
"""
        boot = prefix.with_suffix(".rom.S")
        boot.write_text(rom)
        command(a.cross+"gcc", "-c", "-m68040", boot, "-o", prefix.with_suffix(".rom.o"))
        command(a.cross+"ld", "-m", "m68kelf", "-Ttext", "0xf80000", "-e", "0xf80008", prefix.with_suffix(".rom.o"), "-o", prefix.with_suffix(".rom.elf"))
        command(a.cross+"objcopy", "-O", "binary", "-j", ".text", prefix.with_suffix(".rom.elf"), prefix.with_suffix(".rom"))
        raw = prefix.with_suffix(".rom").read_bytes().ljust(512*1024, b"\xff")
        # A valid Amiga ROM checksum prevents ROM repair from changing vectors.
        raw = raw[:-24] + b"\0\0\0\0" + struct.pack(">I", len(raw)) + raw[-16:]
        total = 0
        for word in struct.unpack(">131072I", raw):
            total += word
            total = (total & 0xffffffff) + (total >> 32)
        raw = raw[:-24] + struct.pack(">I", total ^ 0xffffffff) + raw[-20:]
        prefix.with_suffix(".rom").write_bytes(raw)
        manifest[name] = {"bootstrap_cpu": a.bootstrap_cpu, "iterations": n, "warmup": warm, "expected": expected(n) & 0xffffffff,
                          "payload_sha256": hashlib.sha256(data).hexdigest(),
                          "elf_sha256": hashlib.sha256(prefix.with_suffix('.elf').read_bytes()).hexdigest(),
                          "rom_sha256": hashlib.sha256(raw).hexdigest()}
    (a.out/"manifest.json").write_text(json.dumps(manifest, indent=2)+"\n")


if __name__ == "__main__":
    main()

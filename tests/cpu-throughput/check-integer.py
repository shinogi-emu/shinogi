#!/usr/bin/env python3
"""Check quick-arithmetic results, CCR, branches and preserved X on QEMU."""
import argparse
from pathlib import Path
import random
import selectors
import signal
import subprocess
import time


def flags(value, amount, bits, subtract):
    mask = (1 << bits)-1
    old = value & mask
    result = (old-amount if subtract else old+amount) & mask
    carry = old < amount if subtract else old+amount > mask
    sign = 1 << (bits-1)
    overflow = bool(((old ^ amount) & (old ^ result) if subtract else
                     ~(old ^ amount) & (old ^ result)) & sign)
    ccr = 17*carry + 8*bool(result & sign) + 4*(result == 0) + 2*overflow
    return (value & ~mask) | result, ccr


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--qemu", required=True, type=Path)
    p.add_argument("--cpu", default="m68060")
    p.add_argument("--machine", default="virt")
    p.add_argument("--cross", default="m68k-atari-mintelf-")
    p.add_argument("--out", type=Path, required=True)
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=False)
    lines = [".text", ".globl _start", "_start:", "lea 0x00aff000,%sp"]
    count = 0
    rng = random.Random(68060)
    for bits, suffix in ((8, "b"), (16, "w"), (32, "l")):
        sign = 1 << (bits-1)
        mask = (1 << bits)-1
        values = [0, 1, 7, 8, sign-1, sign, sign+1, mask-1, mask]
        values += [rng.getrandbits(32) for _ in range(12)]
        for subtract in (False, True):
            for amount in range(1, 9):
                for value in values:
                    value = (value | 0xa55a0000) if bits < 32 else value
                    result, ccr = flags(value, amount, bits, subtract)
                    op = "subq" if subtract else "addq"
                    # A taken branch leaves the lazy flags across a TB boundary.
                    lines += [f"movel #{value},%d0", "movew #31,%ccr",
                              f"{op}{suffix} #{amount},%d0", "bra 1f", "1:",
                              "movew %ccr,%d1", f"cmpiw #{ccr},%d1", "bnel fail",
                              f"cmpil #{result & 0xffffffff},%d0", "bnel fail"]
                    count += 1
    # Test all 16 branch conditions directly on implicit-one lazy flags.
    predicates = [lambda n,z,v,c: True, lambda n,z,v,c: False,
                  lambda n,z,v,c: not(c or z), lambda n,z,v,c: c or z,
                  lambda n,z,v,c: not c, lambda n,z,v,c: c,
                  lambda n,z,v,c: not z, lambda n,z,v,c: z,
                  lambda n,z,v,c: not v, lambda n,z,v,c: v,
                  lambda n,z,v,c: not n, lambda n,z,v,c: n,
                  lambda n,z,v,c: n == v, lambda n,z,v,c: n != v,
                  lambda n,z,v,c: not z and n == v, lambda n,z,v,c: z or n != v]
    names = ("t", "f", "hi", "ls", "cc", "cs", "ne", "eq", "vc", "vs",
             "pl", "mi", "ge", "lt", "gt", "le")
    for subtract in (False, True):
        for value in (0, 1, 0x7fffffff, 0x80000000, 0xffffffff, 0x80000001):
            _, ccr = flags(value, 1, 32, subtract)
            for cond, predicate in zip(names, predicates):
                expected = 255 if predicate(bool(ccr&8), bool(ccr&4), bool(ccr&2), bool(ccr&1)) else 0
                op = "subql" if subtract else "addql"
                lines += [f"movel #{value},%d0", "moveq #0,%d1", f"{op} #1,%d0",
                          f"s{cond} %d1", f"cmpil #{expected},%d1", "bnel fail"]
                count += 1
            # Reading CCR synchronizes env->cc_op. BTST then changes only Z.
            op = "subql" if subtract else "addql"
            for amount in range(1, 9):
                result, quick_ccr = flags(value, amount, 32, subtract)
                after_btst = (quick_ccr & ~4) | (4 if not result & 1 else 0)
                lines += [f"movel #{value},%d0", f"{op} #{amount},%d0", "movew %ccr,%d1",
                          "btst #0,%d0", "bra 1f", "1:", "movew %ccr,%d1",
                          f"cmpiw #{after_btst},%d1", "bnel fail"]
                count += 1
            # MOVE clears C/N/V/Z but must preserve X, also across TBs.
            lines += [f"movel #{value},%d0", f"{op} #1,%d0", "moveq #0,%d2",
                      "bra 1f", "1:", "movew %ccr,%d1",
                      f"cmpiw #{(ccr & 16) | 4},%d1", "bnel fail"]
            count += 1
            # ADDX consumes X and the sticky Z set by MOVEQ.
            lines += [f"movel #{value},%d0", f"{op} #1,%d0", "moveq #0,%d2",
                      "moveq #0,%d3", "addxl %d2,%d3", "movew %ccr,%d1",
                      f"cmpiw #{0 if ccr & 16 else 4},%d1", "bnel fail",
                      f"cmpil #{bool(ccr & 16)*1},%d3", "bnel fail"]
            count += 1
    lines += ["moveq #80,%d0", "bra emit", "fail:", "moveq #70,%d0", "emit:",
              "movel %d0,0xff008000", "moveq #10,%d0", "movel %d0,0xff008000",
              "1: stop #0x2700", "bra 1b"]
    source = a.out/"integer-flags.S"
    source.write_text("\n".join(lines)+"\n")
    obj, elf = source.with_suffix(".o"), source.with_suffix(".elf")
    subprocess.run([a.cross+"gcc", "-c", "-m68040", str(source), "-o", str(obj)], check=True)
    subprocess.run([a.cross+"ld", "-m", "m68kelf", "-Ttext", "0x1000", "-e", "_start", str(obj), "-o", str(elf)], check=True)
    cmd = [str(a.qemu.resolve()), "-M", a.machine, "-cpu", a.cpu, "-m", "16",
           "-kernel", str(elf), "-display", "none", "-monitor", "none", "-serial", "stdio"]
    output = b""
    with (a.out/"stderr.log").open("wb") as err:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=err)
        selector = selectors.DefaultSelector()
        selector.register(proc.stdout, selectors.EVENT_READ)
        try:
            deadline = time.monotonic()+30
            while b"\n" not in output and time.monotonic() < deadline:
                if selector.select(.1):
                    chunk = proc.stdout.read1(4096)
                    if not chunk:
                        break
                    output += chunk
        finally:
            proc.send_signal(signal.SIGINT)
            proc.wait(timeout=5)
            selector.close()
            proc.stdout.close()
    (a.out/"stdout.log").write_bytes(output)
    assert output == b"P\n", f"bad completion {output!r}; see {a.out}"
    print(f"PASS: {count} arithmetic/CCR/condition/X checks on {a.cpu}")


if __name__ == "__main__":
    main()

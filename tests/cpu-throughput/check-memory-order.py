#!/usr/bin/env python3
"""Check emitted StoreLoad fences around real m68k memory operations on x86."""
import argparse
from pathlib import Path
import platform
import re
import selectors
import signal
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--qemu', type=Path, required=True)
p.add_argument('--cpu', default='m68060')
p.add_argument('--machine', default='virt')
p.add_argument('--cross', default='m68k-atari-mintelf-')
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
if platform.machine() != 'x86_64':
    p.error('the exact StoreLoad fence assertions require an x86-64 host')
a.out.mkdir(parents=True, exist_ok=False)
source = a.out/'order.S'
source.write_text('''
.text
.globl _start
_start:
lea 0x00aff000,%sp
lea 0x00400000,%a0
lea 0x00500000,%a1
'''+''.join(f'movel #{256+i},{4*i}(%a0)\n' for i in range(8))+'''
bsr load_only
'''+''.join(f'cmpil #{256+i},%d{i}\nbne fail\n' for i in range(8))+'''
bsr load_store_load
cmpil #257,%d1
bne fail
bsr load_helper_load
cmpil #257,%d1
bne fail
bsr load_branch_load
cmpil #258,%d2
bne fail
bsr load_mmio
moveq #80,%d0
bra emit
fail:
moveq #70,%d0
emit:
movel %d0,0xff008000
moveq #10,%d0
movel %d0,0xff008000
1: stop #0x2700
bra 1b
load_only:
moveml (%a0),%d0-%d7
rts
load_store_load:
movel (%a0),%d0
movel %d0,(%a1)
movel 4(%a0),%d1
rts
load_helper_load:
movel (%a0),%d0
movew %ccr,%d2
movel 4(%a0),%d1
rts
load_branch_load:
movel (%a0),%d0
bne branch_target
movel 4(%a0),%d1
branch_target:
movel 8(%a0),%d2
rts
load_mmio:
lea 0xff008000,%a2
movel 4(%a2),%d0
movel 4(%a2),%d1
rts
''')
obj, elf = source.with_suffix('.o'), source.with_suffix('.elf')
subprocess.run([a.cross+'gcc', '-c', '-m68040', str(source), '-o', str(obj)], check=True)
subprocess.run([a.cross+'ld', '-m', 'm68kelf', '-Ttext', '0x1000', '-e', '_start', str(obj), '-o', str(elf)], check=True)
symbols = {name: int(address, 16) for address, _, name in
           (line.split() for line in subprocess.check_output([a.cross+'nm', '-n', str(elf)], text=True).splitlines())}
trace = a.out/'trace.log'
cmd = [str(a.qemu.resolve()), '-M', a.machine, '-cpu', a.cpu, '-m', '16',
       '-kernel', str(elf), '-display', 'none', '-monitor', 'none', '-serial', 'stdio',
       '-d', 'op_opt', '-D', str(trace)]
output = b''
with (a.out/'stderr.log').open('wb') as err:
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=err)
    selector = selectors.DefaultSelector()
    selector.register(proc.stdout, selectors.EVENT_READ)
    try:
        deadline = time.monotonic()+20
        while b'\n' not in output and time.monotonic() < deadline:
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
assert output == b'P\n', f'guest failed: {output!r}'
blocks = trace.read_text().split('OP after optimization and liveness analysis:\n')[1:]

def block(name):
    address = symbols[name]
    matches = [b for b in blocks if re.search(r'\n ---- 0*'+f'{address:x}'+r' ', b)]
    assert len(matches) == 1, (name, len(matches))
    return matches[0]

for name, loads, stores, fences in (('load_only', 9, 0, 1),
                                  ('load_store_load', 3, 1, 2),
                                  ('load_helper_load', 3, 0, 2),
                                  ('branch_target', 2, 0, 1),
                                  ('load_mmio', 3, 0, 1)):
    b = block(name)
    actual = (len(re.findall(r'^ qemu_ld_', b, re.M)),
              len(re.findall(r'^ qemu_st_', b, re.M)),
              len(re.findall(r'^ mb seq:wr', b, re.M)))
    assert actual == (loads, stores, fences), (name, actual)
    if name == 'load_helper_load':
        assert re.search(r'mb seq:wr.*call get_ccr.*mb seq:wr', b, re.S)
    if name == 'load_store_load':
        assert re.search(r'mb seq:wr.*qemu_st_.*mb seq:wr', b, re.S)
print('PASS: consecutive loads, intervening store/helper, branch and MMIO fence checks')

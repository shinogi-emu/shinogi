#!/usr/bin/env python3
"""Migrate implicit quick-arithmetic flags to a receiver without the new encoding."""
import argparse
import json
from pathlib import Path
import signal
import socket
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--qemu', type=Path, required=True)
p.add_argument('--receiver', type=Path, required=True)
p.add_argument('--cpu', default='m68060')
p.add_argument('--machine', default='virt')
p.add_argument('--cross', default='m68k-atari-mintelf-')
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=False)

class QMP:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        deadline = time.monotonic()+10
        while True:
            try:
                self.sock.connect(str(path))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() > deadline:
                    raise
                time.sleep(.01)
        self.sock.settimeout(10)
        self.stream = self.sock.makefile('rwb', buffering=0)
        assert 'QMP' in json.loads(self.stream.readline())
        self.execute('qmp_capabilities')

    def execute(self, command, arguments=None):
        self.stream.write((json.dumps({'execute': command, 'arguments': arguments or {}})+'\n').encode())
        while True:
            response = json.loads(self.stream.readline())
            if 'error' in response:
                raise RuntimeError(response)
            if 'return' in response:
                return response['return']

    def close(self):
        self.stream.close()
        self.sock.close()


def wait_for(path, marker, timeout):
    deadline = time.monotonic()+timeout
    while time.monotonic() < deadline:
        if path.exists() and marker in path.read_bytes():
            return
        time.sleep(.01)
    raise RuntimeError(f'missing {marker!r} in {path}')

for op, value, expected in (('addql', 0xffffffff, 0x15), ('addql', 0x7fffffff, 0x0a),
                            ('subql', 0, 0x19), ('subql', 0x80000000, 0x02)):
    work = a.out/f'{op}-{value:x}'
    work.mkdir()
    source = work/'flags.S'
    source.write_text(f'''
.text
.globl _start
_start:
lea 0x00aff000,%sp
moveq #-1,%d6
moveq #-1,%d7
moveq #66,%d0
movel %d0,0xff008000
movel #{value},%d4
{op} #1,%d4
/* DBRA preserves all flags, including lazy V/C/X, throughout the pause. */
1: dbra %d7,1b
   dbra %d6,1b
movew %ccr,%d1
cmpiw #{expected},%d1
bne fail
moveq #80,%d0
bra emit
fail:
moveq #70,%d0
emit:
movel %d0,0xff008000
2: stop #0x2700
bra 2b
''')
    obj, elf = source.with_suffix('.o'), source.with_suffix('.elf')
    subprocess.run([a.cross+'gcc', '-c', '-m68040', str(source), '-o', str(obj)], check=True)
    subprocess.run([a.cross+'ld', '-m', 'm68kelf', '-Ttext', '0x1000', '-e', '_start', str(obj), '-o', str(elf)], check=True)
    common = ['-M', a.machine, '-cpu', a.cpu, '-m', '16', '-display', 'none', '-monitor', 'none']
    state = work/'state'
    src = dst = qmp = None
    with (work/'source.err').open('wb') as err, (work/'receiver.err').open('wb') as err2:
        try:
            src = subprocess.Popen([str(a.qemu.resolve()), *common, '-kernel', str(elf),
                                    '-serial', 'file:'+str(work/'source.log'),
                                    '-qmp', 'unix:'+str(work/'qmp')+',server=on,wait=off'], stderr=err)
            qmp = QMP(work/'qmp')
            wait_for(work/'source.log', b'B', 10)
            time.sleep(.05)
            qmp.execute('stop')
            assert (work/'source.log').read_bytes() == b'B', 'guest already finished'
            qmp.execute('migrate', {'uri': 'file:'+str(state)})
            deadline = time.monotonic()+20
            while True:
                migration = qmp.execute('query-migrate')
                if migration['status'] == 'completed':
                    break
                assert migration['status'] != 'failed', migration
                assert time.monotonic() < deadline, migration
                time.sleep(.01)
            qmp.execute('quit')
            src.wait(timeout=5)
            qmp.close()
            qmp = None
            dst = subprocess.Popen([str(a.receiver.resolve()), *common,
                                    '-serial', 'file:'+str(work/'receiver.log'),
                                    '-incoming', 'file:'+str(state),
                                    '-qmp', 'unix:'+str(work/'qmp2')+',server=on,wait=off'], stderr=err2)
            qmp = QMP(work/'qmp2')
            qmp.execute('cont')
            wait_for(work/'receiver.log', b'P', 25)
            assert (work/'receiver.log').read_bytes() == b'P'
            qmp.execute('quit')
            dst.wait(timeout=5)
            print(f'PASS: {op} {value:#x}, migrated CCR={expected:#x}', flush=True)
        finally:
            if qmp:
                qmp.close()
            for proc in (src, dst):
                if proc is not None and proc.poll() is None:
                    proc.send_signal(signal.SIGINT)
                    proc.wait(timeout=5)

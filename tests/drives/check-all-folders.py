#!/usr/bin/env python3
"""Exercise all 23 production folder channels, including a non-C first port."""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile
import time
p = argparse.ArgumentParser(description=__doc__)
for arg in ['firmware', 'guest', 'probe', 'qemu']: p.add_argument('--'+arg, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
w = Path(tempfile.mkdtemp(prefix='shinogi-all-folders-'))
shutil.copytree(a.guest, w/'c')
shutil.copy2(a.probe, w/'c/AUTO/00DRIVES.PRG')
shutil.copy2(a.probe, w/'c/ALL.TTP')
cnf = w/'c/MINT/1-19-CUR/MINT.CNF'; cnf.write_text(cnf.read_text()+'\nexec u:/c/ALL.TTP mint\n')
command = [a.qemu, '-M', 'virt', '-cpu', 'm68060', '-m', '128', '-kernel', a.firmware,
           '-device', 'virtio-gpu-device', '-device', 'virtio-keyboard-device', '-device', 'virtio-tablet-device',
           '-device', 'virtio-serial-device,id=folders,max_ports=26']
processes = []
try:
    for letter in reversed('CDEFGHIJKLMNOPQRSTVWXYZ'):
        folder = w/letter.lower(); folder.mkdir(exist_ok=True)
        (folder/'SAME.TXT').write_text(f'{letter} root\n')
        ready = w/(letter+'.ready'); sock = w/(letter+'.sock')
        args = [str(root/'tools/hostfsd/shinogi-hostfsd'), '--root', str(folder), '--listen', str(sock), '--ready-file', str(ready), '--once']
        if letter != 'C': args += ['--read-only']
        proc = subprocess.Popen(args, stdout=(w/(letter+'.log')).open('wb'), stderr=subprocess.STDOUT)
        processes.append(proc)
        for _ in range(300):
            if ready.exists(): break
            assert proc.poll() is None
            time.sleep(.01)
        assert ready.exists()
        name = 'shinogi.hostfs' if letter == 'C' else 'shinogi.hostfs.'+letter.lower()
        command += ['-chardev', f'socket,id={letter},path={sock},server=off', '-device', f'virtserialport,bus=folders.0,chardev={letter},name={name}']
    command += ['-display', 'none', '-serial', f'file:{w}/serial.log']
    q = subprocess.Popen(command, stdout=(w/'qemu.log').open('wb'), stderr=subprocess.STDOUT)
    processes.append(q)
    deadline = time.monotonic()+75
    while time.monotonic() < deadline:
        assert q.poll() is None
        log = w/'c/ALLMINT.LOG'
        if log.exists() and ('PASS' in log.read_text() or 'FAIL' in log.read_text()): break
        time.sleep(.1)
    for name in ['ALLEMUTO.LOG', 'ALLMINT.LOG']:
        text = (w/'c'/name).read_text()
        print(name, text.strip(), flush=True)
        assert text.strip() == 'PASS', text
    print('PASS all 23 mapped folders on EmuTOS and FreeMiNT', w)
finally:
    for proc in reversed(processes):
        if proc.poll() is None: proc.terminate(); proc.wait(timeout=10)

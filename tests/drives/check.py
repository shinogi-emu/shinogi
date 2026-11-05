#!/usr/bin/env python3
"""Disposable EmuTOS + FreeMiNT multi-drive read/write/reopen integration.
Requires a compiled probe, current firmware, guest C tree and Shinogi QEMU.
All mutable test files are created below a new work directory.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import socket
import json
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser()
p.add_argument('--firmware', required=True)
p.add_argument('--guest', required=True)
p.add_argument('--probe', required=True)
p.add_argument('--qemu', required=True)
p.add_argument('--work')
a = p.parse_args()
work = Path(a.work) if a.work else Path(tempfile.mkdtemp(prefix='shinogi-drives-'))
work.mkdir(exist_ok=True)
helper = ROOT / 'tools/hostfsd/shinogi-hostfsd'


def run(*args):
    subprocess.run(args, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot(folder):
    return {str(x.relative_to(folder)): (digest(x), x.stat().st_mode & 0o777)
            for x in folder.rglob('*') if x.is_file()}


def qmp(file, command):
    with socket.socket(socket.AF_UNIX) as sock:
        sock.connect(str(file))
        stream = sock.makefile('rwb', buffering=0)
        json.loads(stream.readline())
        for cmd in ['qmp_capabilities', command]:
            stream.write((json.dumps({'execute': cmd})+'\n').encode())
            while True:
                reply = json.loads(stream.readline())
                if 'return' in reply: break
                if 'error' in reply: raise RuntimeError(reply)


boot = work / 'c'
shutil.copytree(a.guest, boot)
shutil.copy2(a.probe, boot/'AUTO/00DRIVES.PRG')
shutil.copy2(a.probe, boot/'DRIVES.TTP')
cnf = boot / 'MINT/1-19-CUR/MINT.CNF'
cnf.write_text(cnf.read_text()+'\nexec u:/c/DRIVES.TTP mint\n')
folders = {'C': boot}
for letter in ['D', 'E', 'Z']:
    folder = work / letter.lower()
    folder.mkdir()
    folders[letter] = folder
for letter, folder in folders.items():
    (folder/'SAME.TXT').write_text(f'{letter} root\n')
    for n in range(37): (folder/f'X{n:03}.TXT').write_text(f'{letter}{n}\n')
(folders['D']/'SUB').mkdir()
(folders['D']/'SUB/LOCAL.TXT').write_text('D sub\n')
(folders['Z']/'OTHER').mkdir()
(folders['Z']/'OTHER/LOCAL.TXT').write_text('Z other\n')
(folders['E']/'SAME.TXT').chmod(0o444)
original_e = snapshot(folders['E'])
images = {}
for letter, size, serial, ro in [('F', 32*1024*1024, 'shinogi.disk1', False),
                                 ('G', 32*1024*1024, 'shinogi.disk2', True),
                                 ('A', 737280, 'shinogi.floppya', False),
                                 ('B', 1474560, 'shinogi.floppyb', True)]:
    image = work / (letter+'.img')
    with image.open('wb') as out: out.truncate(size)
    run('mkfs.fat', '-F', '12' if size < 2000000 else '16', str(image))
    text = work / 'SAME.TXT'; text.write_text(f'{letter} root\n')
    run('mcopy', '-i', str(image), str(text), '::SAME.TXT')
    if ro: run('mattrib', '-i', str(image), '+r', '::SAME.TXT')
    text.write_text(f'{letter} untouched\n')
    run('mcopy', '-i', str(image), str(text), '::KEEP.TXT')
    images[letter] = (image, serial, ro)
original_ro = {l: digest(i[0]) for l, i in images.items() if i[2]}
payload = bytes((i*37+i//251)&255 for i in range(24577))


def boot_once(label, firmware):
    out = work / label; out.mkdir()
    processes = []
    emulator = None
    try:
        command = [a.qemu, '-M', 'virt', '-cpu', 'm68060', '-m', '128', '-kernel', firmware,
                   '-netdev', 'user,id=n0,ipv6=off', '-device', 'virtio-net-device,netdev=n0',
                   '-device', 'virtio-gpu-device,xres=1024,yres=768', '-device', 'virtio-keyboard-device',
                   '-device', 'virtio-tablet-device', '-device', 'virtio-serial-device,id=folders,max_ports=26']
        # Deliberately scramble port order to prove names determine letters.
        for letter in ['Z', 'E', 'C', 'D']:
            sock = out / (letter+'.sock'); ready = out / (letter+'.ready')
            args = [str(helper), '--root', str(folders[letter]), '--listen', str(sock),
                    '--ready-file', str(ready), '--once']
            if letter == 'E': args += ['--read-only']
            proc = subprocess.Popen(args, stdout=(out/(letter+'.log')).open('wb'), stderr=subprocess.STDOUT)
            processes.append(proc)
            for _ in range(200):
                if ready.exists(): break
                assert proc.poll() is None
                time.sleep(.01)
            assert ready.exists(), 'helper readiness timed out'
            name = 'shinogi.hostfs' if letter == 'C' else 'shinogi.hostfs.'+letter.lower()
            command += ['-chardev', f'socket,id=fs{letter},path={sock},server=off', '-device',
                        f'virtserialport,bus=folders.0,chardev=fs{letter},name={name}']
        # Reverse image command-line order as a separate identity control.
        for letter in ['B', 'G', 'A', 'F']:
            image, serial, ro = images[letter]
            command += ['-drive', f'file={image},format=raw,if=none,id=img{letter},readonly={"on" if ro else "off"},cache=writethrough',
                        '-device', f'virtio-blk-device,drive=img{letter},serial={serial}']
        command += ['-display', 'none', '-serial', f'file:{out}/serial.log',
                    '-qmp', f'unix:{out}/qmp.sock,server=on,wait=off']
        emulator = subprocess.Popen(command, stdout=(out/'qemu.log').open('wb'), stderr=subprocess.STDOUT)
        deadline = time.monotonic()+75
        while time.monotonic() < deadline:
            assert emulator.poll() is None, (out/'qemu.log').read_text()
            result = boot/'MINTDRV.LOG'
            if result.exists() and ('MINT PASS' in result.read_text() or 'MINT FAIL' in result.read_text()): break
            time.sleep(.1)
        else: raise AssertionError('guest probe timed out; inspect '+str(out))
        for file in ['EMUDRV.LOG', 'MINTDRV.LOG']:
            text = (boot/file).read_text(); (out/file).write_text(text)
            print(label, file, text.strip(), flush=True)
            assert 'FAIL' not in text and 'PASS' in text, str(out/file)
        qmp(out/'qmp.sock', 'quit'); emulator.wait(timeout=10)
        assert emulator.returncode == 0
    finally:
        for proc in [emulator]+processes:
            if proc is not None and proc.poll() is None:
                proc.terminate(); proc.wait(timeout=10)


for attempt in range(3):
    if attempt == 2:
        # Swap both media and geometries between emulator runs. Named A/B
        # identities must survive different transport ordering and capacity.
        (boot/'SWAP.TAG').write_text('swapped')
        for letter, size in [('A', 1474560), ('B', 737280)]:
            old, serial, ro = images[letter]
            image = work / (letter+'-swapped.img')
            with image.open('wb') as out: out.truncate(size)
            run('mkfs.fat', '-F', '12', str(image))
            text = work / 'SAME.TXT'; text.write_text(f'{letter} swap\n')
            run('mcopy', '-i', str(image), str(text), '::SAME.TXT')
            if ro: run('mattrib', '-i', str(image), '+r', '::SAME.TXT')
            text.write_text(f'{letter} untouched\n')
            run('mcopy', '-i', str(image), str(text), '::KEEP.TXT')
            if not ro:
                data = work/'payload.bin'; data.write_bytes(payload)
                for name in ['RESULT.BIN', 'SECOND.BIN']:
                    run('mcopy', '-i', str(image), str(data), '::'+name)
            images[letter] = image, serial, ro
            if ro: original_ro[letter] = digest(image)
    # Fresh logs are necessary: a previous PASS must not satisfy a new run.
    for name in ['EMUDRV.LOG', 'MINTDRV.LOG']:
        path = boot / name
        if path.exists(): path.rename(work/f'previous-{attempt}-{name}')
    boot_once(f'run{attempt}', a.firmware)
    assert snapshot(folders['E']) == original_e, 'read-only folder content/attributes changed'
    for letter, (image, serial, ro) in images.items():
        if ro: assert digest(image) == original_ro[letter], f'{letter}: read-only image modified'
        else:
            for name in ['RESULT.BIN', 'SECOND.BIN']:
                data = subprocess.check_output(['mtype', '-i', str(image), '::'+name])
                assert data == payload, (letter, name, len(data))
            keep = subprocess.check_output(['mtype', '-i', str(image), '::KEEP.TXT'])
            assert keep == f'{letter} untouched\n'.encode()
            run('fsck.fat', '-n', str(image))
    for letter in ['D', 'Z']:
        for name in ['RESULT.BIN', 'SECOND.BIN']: assert (folders[letter]/name).read_bytes() == payload
        assert not (folders[letter]/'MUTATE').exists()
print('PASS: named folder contexts, handles/cwd/DTA isolation, raw images, floppy A/B, writes, read-only hashes, reopen and fsck', work)

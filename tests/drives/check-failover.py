#!/usr/bin/env python3
"""Kill D's real helper with files open on C/D/Z; verify isolated failure."""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile
import time
p=argparse.ArgumentParser(description=__doc__)
for arg in ['firmware','guest','probe','qemu']: p.add_argument('--'+arg, required=True)
a=p.parse_args(); root=Path(__file__).resolve().parents[2]
w=Path(tempfile.mkdtemp(prefix='shinogi-drive-failover-'))
shutil.copytree(a.guest, w/'c'); shutil.copy2(a.probe, w/'c/AUTO/00FAIL.PRG')
processes=[]; helpers={}
command=[a.qemu,'-M','virt','-cpu','m68060','-m','128','-kernel',a.firmware,
         '-device','virtio-gpu-device','-device','virtio-keyboard-device','-device','virtio-tablet-device',
         '-device','virtio-serial-device,id=folders,max_ports=26']
try:
    for letter in ['C','D','Z']:
        folder=w/letter.lower(); folder.mkdir(exist_ok=True)
        (folder/'SAME.TXT').write_text(f'{letter} root\n')
        ready=w/(letter+'.ready'); sock=w/(letter+'.sock')
        proc=subprocess.Popen([str(root/'tools/hostfsd/shinogi-hostfsd'),'--root',str(folder),'--listen',str(sock),'--ready-file',str(ready),'--once'],stdout=(w/(letter+'.log')).open('wb'),stderr=subprocess.STDOUT)
        processes.append(proc); helpers[letter]=proc
        for _ in range(200):
            if ready.exists(): break
            assert proc.poll() is None
            time.sleep(.01)
        assert ready.exists()
        name='shinogi.hostfs' if letter=='C' else 'shinogi.hostfs.'+letter.lower()
        command+=['-chardev',f'socket,id={letter},path={sock},server=off','-device',f'virtserialport,bus=folders.0,chardev={letter},name={name}']
    command+=['-display','none','-serial',f'file:{w}/serial.log']
    q=subprocess.Popen(command,stdout=(w/'qemu.log').open('wb'),stderr=subprocess.STDOUT); processes.append(q)
    deadline=time.monotonic()+60; killed=False
    while time.monotonic()<deadline:
        assert q.poll() is None
        if not killed and (w/'c/KILL.TAG').exists():
            helpers['D'].terminate(); helpers['D'].wait(timeout=10)
            (w/'c/GO.TAG').write_text('continue'); killed=True
        log=w/'c/FAILOVER.LOG'
        if log.exists() and ('PASS' in log.read_text() or 'FAIL' in log.read_text()): break
        time.sleep(.02)
    assert killed
    assert (w/'c/FAILOVER.LOG').read_text().strip()=='PASS'
    assert 'hostfs-link: channel dead' in (w/'serial.log').read_text()
    assert (w/'c/SAME.TXT').read_text()=='C root\n' and (w/'z/SAME.TXT').read_text()=='Z root\n'
    print('PASS: bounded dead-D failure, persistent routing, overlapping handles and live C/Z reads',w)
finally:
    for proc in reversed(processes):
        if proc.poll() is None: proc.terminate(); proc.wait(timeout=10)

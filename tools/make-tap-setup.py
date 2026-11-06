#!/usr/bin/env python3
"""Package the standalone signed adapter setup for both Atari emulators."""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]


def build(output, label):
    name = 'Atari-TAP-Setup-' + label
    archive = output / (name + '.zip')
    if archive.exists() or archive.with_suffix('.zip.sha256').exists():
        raise ValueError('Refusing to overwrite a versioned setup package')
    spec = importlib.util.spec_from_file_location('network_inputs', ROOT / 'tools/fetch-network-inputs.py')
    inputs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(inputs)
    with tempfile.TemporaryDirectory(prefix='atari-tap-') as temporary:
        stage = Path(temporary)
        inputs.stage(stage, tap_only=True)
        folder = stage / 'network'
        subprocess.run(['x86_64-w64-mingw32-gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-O2', '-municode', str(ROOT / 'tools/win/install-tap.c'), '-o', str(folder / 'install-tap.exe'),
                        '-lsetupapi', '-lnewdev', '-ladvapi32', '-lole32'], check=True)
        shutil.copyfile(ROOT / 'tools/win/install-tap.c', folder / 'source/install-tap.c')
        for file in ('tap-adapter.ps1', 'tap-setup.ps1', 'Atari-TAP-Setup.cmd',
                     'collect-wifi.ps1', 'Collect-WiFi-LAN.cmd', 'lan-setup.ps1'):
            shutil.copyfile(ROOT / 'tools/win' / file, folder / file)
        (folder / 'README.txt').write_text(
            'Atari standalone TAP setup for Windows x64\n'
            '========================================\n\n'
            'Run Atari-TAP-Setup.cmd and choose Both, Shinogi, or ARAnyM.\n'
            'Approve the Windows administrator prompt. The signed TAP driver\n'
            'is installed directly; OpenVPN is not installed or required.\n'
            'Each emulator gets a separate adapter. Existing VPN adapters,\n'
            'Wi-Fi settings, addresses, routes and DNS are preserved.\n\n'
            'If an existing VPN uses another TAP driver build, setup stops\n'
            'rather than upgrading or restarting that shared driver.\n\n'
            'Installing an adapter is not the same as connecting it to your LAN.\n'
            'Shinogi Settings > LAN > Set up LAN handles wired DHCP Ethernet.\n'
            'ARAnyM needs the accompanying updated Windows TAP handler.\n'
            'The driver must not be shared by two simultaneously running guests\n'
            'through the same adapter; each active guest needs its own TAP.\n\n'
            'Wi-Fi LAN is not enabled by this package. Compatibility depends on\n'
            'the Windows version, Wi-Fi driver and access point. Run\n'
            'Collect-WiFi-LAN.cmd to save a PRIVATE read-only report for the\n'
            'next compatibility test. It changes no driver or networking settings.\n'
            'Keep using Internet/SLIRP on Wi-Fi until the LAN test is validated.\n\n'
            'Driver/source versions and hashes are in PROVENANCE.txt.\n'
            'Licenses are in tap/ and matching source archives in source/.\n', encoding='utf-8')
        entries = sorted(path for path in folder.rglob('*') if path.is_file())
        (folder / 'CONTENTS.sha256').write_text(''.join(
            hashlib.sha256(path.read_bytes()).hexdigest() + '  ' + path.relative_to(folder).as_posix() + '\n'
            for path in entries), encoding='ascii')
        output.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED) as target:
            for path in sorted(folder.rglob('*')):
                if path.is_file():
                    target.write(path, name + '/' + path.relative_to(folder).as_posix())
        with zipfile.ZipFile(archive) as target:
            if target.testzip() is not None:
                raise ValueError('Corrupt setup archive')
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        archive.with_suffix('.zip.sha256').write_text(digest + '  ' + archive.name + '\n', encoding='ascii')
    print(archive)
    print(digest)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--label', required=True)
    args = parser.parse_args()
    if not args.label or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.' for c in args.label):
        parser.error('label must contain only letters, digits, hyphens or periods')
    build(args.output, args.label)

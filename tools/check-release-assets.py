#!/usr/bin/env python3
"""Validate the exact public edition assets and their download checksums."""
import argparse
import hashlib
from pathlib import Path
import plistlib
import tarfile
import zipfile

EDITIONS = {'Shinogi-040': 'm68040', 'Shinogi-060': 'm68060'}
PLATFORMS = {'windows': 'win64-setup.exe', 'macos': 'macos-arm64.zip',
             'linux': 'linux-x86_64.tar.gz'}


def validate_linux(file, edition, version):
    root = f'{edition}-{version}-linux-x86_64'
    with tarfile.open(file, 'r:gz') as archive:
        members = archive.getmembers()
        names = set()
        for member in members:
            parts = member.name.split('/')
            if parts[0] != root or '..' in parts or not (member.isfile() or member.isdir()):
                raise ValueError(f'{file.name}: unsafe archive member {member.name}')
            if member.name in names:
                raise ValueError(f'{file.name}: duplicate archive member {member.name}')
            names.add(member.name)

        def read(name):
            try:
                member = archive.getmember(root+'/'+name)
            except KeyError:
                raise ValueError(f'{file.name}: missing {name}') from None
            if not member.isfile():
                raise ValueError(f'{file.name}: not a regular file: {name}')
            return archive.extractfile(member).read()

        metadata = dict(line.split(': ', 1) for line in read('BUILD.txt').decode().splitlines() if ': ' in line)
        if metadata.get('Edition') != edition or metadata.get('CPU') != EDITIONS[edition]:
            raise ValueError(f'{file.name}: wrong Linux edition identity')
        for name in ('shinogi', 'bin/qemu-system-m68k', 'bin/shinogi-hostfsd',
                     'bin/shinogi-monitor', 'bin/shinogi-shot', 'lib/glibc/ld-linux-x86-64.so.2'):
            data = read(name)
            if not archive.getmember(root+'/'+name).mode & 0o111:
                raise ValueError(f'{file.name}: not executable: {name}')
            if name == 'shinogi':
                if b'@CPU@' in data or b'@EDITION@' in data or b'@VERSION@' in data:
                    raise ValueError(f'{file.name}: unsubstituted launcher')
            elif data[:6] != b'\x7fELF\x02\x01' or data[18:20] != b'\x3e\x00':
                raise ValueError(f'{file.name}: not an x86_64 ELF: {name}')
        for field, name in (('QEMU', 'bin/qemu-system-m68k'),
                            ('EmuTOS image', 'guest/emutos-virt.elf'),
                            ('FreeMiNT kernel', 'guest/drive-c/AUTO/MINT.PRG'),
                            ('Network driver', 'guest/drive-c/MINT/1-19-CUR/VIRTIONE.XIF')):
            if hashlib.sha256(read(name)).hexdigest() != metadata.get(field+' sha256'):
                raise ValueError(f'{file.name}: {field} payload checksum mismatch')
        has_sp = root+'/guest/drive-c/AUTO/060SP.PRG' in names
        if has_sp != (edition == 'Shinogi-060'):
            raise ValueError(f'{file.name}: wrong 060 software package selection')


def validate(folder, version, platforms=None):
    platforms = tuple(PLATFORMS) if platforms is None else tuple(platforms)
    expected = set()
    for edition, cpu in EDITIONS.items():
        for platform in platforms:
            suffix = PLATFORMS[platform]
            name = f'{edition}-{version}-{suffix}'
            expected.update([name, name+'.sha256'])
    actual = {p.name for p in folder.iterdir()}
    if actual != expected:
        raise ValueError(f'Unexpected release files: missing={sorted(expected-actual)}, extra={sorted(actual-expected)}')
    for name in sorted(expected):
        if name.endswith('.sha256'):
            continue
        file = folder/name
        fields = (folder/(name+'.sha256')).read_text().strip().split(None, 1)
        if len(fields) != 2 or fields[1].lstrip('*') != name:
            raise ValueError(f'{name}: checksum must name the downloaded basename')
        with file.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        if digest != fields[0]:
            raise ValueError(f'{name}: checksum mismatch')
        if name.endswith('.exe'):
            with file.open('rb') as stream:
                if stream.read(2) != b'MZ': raise ValueError(f'{name}: not a Windows executable')
        elif name.endswith('.tar.gz'):
            edition = next(e for e in EDITIONS if name.startswith(e+'-'))
            validate_linux(file, edition, version)
        else:
            edition = next(e for e in EDITIONS if name.startswith(e+'-'))
            with zipfile.ZipFile(file) as archive:
                info = plistlib.loads(archive.read(f'{edition}.app/Contents/Info.plist'))
                if info.get('CFBundleDisplayName') != edition or info.get('CFBundleIdentifier') != 'no.shinogi.emulator.'+EDITIONS[edition]:
                    raise ValueError(f'{name}: wrong app edition identity')
                if archive.testzip() is not None: raise ValueError(f'{name}: corrupt ZIP content')
    return sorted(expected)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    parser.add_argument('version')
    parser.add_argument('--platform', action='append', choices=PLATFORMS,
                        help='validate only this platform (repeatable; default: all)')
    args = parser.parse_args()
    for name in validate(args.folder, args.version, args.platform): print(name)

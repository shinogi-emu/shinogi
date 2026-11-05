#!/usr/bin/env python3
"""Validate the exact public edition assets and their download checksums."""
import argparse
import hashlib
from pathlib import Path
import plistlib
import zipfile

EDITIONS = {'Shinogi-040': 'm68040', 'Shinogi-060': 'm68060'}


def validate(folder, version):
    expected = set()
    for edition, cpu in EDITIONS.items():
        for suffix in ['win64-setup.exe', 'macos-arm64.zip']:
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
    args = parser.parse_args()
    for name in validate(args.folder, args.version): print(name)

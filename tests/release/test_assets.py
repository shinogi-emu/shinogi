import hashlib
import io
import importlib.util
from pathlib import Path
import plistlib
import tempfile
import tarfile
import unittest
import zipfile
import sys
sys.dont_write_bytecode = True
spec=importlib.util.spec_from_file_location('assets', Path(__file__).resolve().parents[2]/'tools/check-release-assets.py')
assets=importlib.util.module_from_spec(spec);spec.loader.exec_module(assets)

class Assets(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.folder=Path(self.tmp.name)
        for edition,cpu in assets.EDITIONS.items():
            for suffix in assets.PLATFORMS.values():
                file=self.folder/f'{edition}-beta15-{suffix}'
                if suffix.endswith('.exe'):file.write_bytes(b'MZfixture')
                elif suffix.endswith('.tar.gz'):self.linux(file, edition, cpu)
                else:
                    with zipfile.ZipFile(file,'w') as archive:
                        archive.writestr(f'{edition}.app/Contents/Info.plist', plistlib.dumps({'CFBundleDisplayName':edition,'CFBundleIdentifier':'no.shinogi.emulator.'+cpu}))
                file.with_name(file.name+'.sha256').write_text(hashlib.sha256(file.read_bytes()).hexdigest()+'  '+file.name+'\n')
    def linux(self, file, edition, cpu, omit=None, unsafe=False):
        elf=b'\x7fELF\x02\x01'+bytes(12)+b'\x3e\x00'+bytes(4)
        files={name:elf for name in ('bin/qemu-system-m68k','bin/shinogi-hostfsd','bin/shinogi-monitor','bin/shinogi-shot','lib/glibc/ld-linux-x86-64.so.2')}
        files.update({'shinogi':b'#!/bin/sh\n', 'guest/emutos-virt.elf':b'guest',
                      'guest/drive-c/AUTO/MINT.PRG':b'kernel',
                      'guest/drive-c/MINT/1-19-CUR/VIRTIONE.XIF':b'net'})
        if edition=='Shinogi-060':files['guest/drive-c/AUTO/060SP.PRG']=b'sp'
        metadata=f'Edition: {edition}\nCPU: {cpu}\n'
        for field,name in (('QEMU','bin/qemu-system-m68k'),('EmuTOS image','guest/emutos-virt.elf'),('FreeMiNT kernel','guest/drive-c/AUTO/MINT.PRG'),('Network driver','guest/drive-c/MINT/1-19-CUR/VIRTIONE.XIF')):
            metadata+=f'{field} sha256: {hashlib.sha256(files[name]).hexdigest()}\n'
        files['BUILD.txt']=metadata.encode()
        if unsafe:files['../escape']=b'unsafe'
        with tarfile.open(file,'w:gz') as archive:
            for name,data in files.items():
                if name==omit:continue
                info=tarfile.TarInfo(f'{edition}-beta15-linux-x86_64/{name}')
                info.mode=0o755;info.size=len(data);archive.addfile(info,io.BytesIO(data))
    def rehash(self,file):
        file.with_name(file.name+'.sha256').write_text(hashlib.sha256(file.read_bytes()).hexdigest()+'  '+file.name+'\n')
    def tearDown(self):self.tmp.cleanup()
    def test_exact_public_set(self):self.assertEqual(len(assets.validate(self.folder,'beta15')),12)
    def test_linux_only(self):
        for p in self.folder.iterdir():
            if 'linux-x86_64' not in p.name:p.unlink()
        self.assertEqual(len(assets.validate(self.folder,'beta15',['linux'])),4)
    def test_wrong_linux_cpu(self):
        file=self.folder/'Shinogi-060-beta15-linux-x86_64.tar.gz'
        self.linux(file,'Shinogi-060','m68040');self.rehash(file)
        with self.assertRaisesRegex(ValueError,'identity'):assets.validate(self.folder,'beta15')
    def test_unsafe_linux_member(self):
        file=self.folder/'Shinogi-060-beta15-linux-x86_64.tar.gz'
        self.linux(file,'Shinogi-060','m68060',unsafe=True);self.rehash(file)
        with self.assertRaisesRegex(ValueError,'unsafe'):assets.validate(self.folder,'beta15')
    def test_missing_linux_helper(self):
        file=self.folder/'Shinogi-060-beta15-linux-x86_64.tar.gz'
        self.linux(file,'Shinogi-060','m68060',omit='bin/shinogi-hostfsd');self.rehash(file)
        with self.assertRaisesRegex(ValueError,'missing'):assets.validate(self.folder,'beta15')
    def test_unexpected_edition(self):
        (self.folder/'OtherCPU-beta15-win64-setup.exe').write_bytes(b'MZ')
        with self.assertRaisesRegex(ValueError,'extra='):assets.validate(self.folder,'beta15')
    def test_missing_package(self):
        file=self.folder/'Shinogi-040-beta15-macos-arm64.zip';file.rename(self.folder/'renamed.zip')
        with self.assertRaises(ValueError):assets.validate(self.folder,'beta15')
    def test_modified_download(self):
        (self.folder/'Shinogi-060-beta15-win64-setup.exe').write_bytes(b'MZchanged')
        with self.assertRaisesRegex(ValueError,'checksum mismatch'):assets.validate(self.folder,'beta15')
    def test_wrong_checksum_filename(self):
        file=self.folder/'Shinogi-060-beta15-macos-arm64.zip.sha256'
        file.write_text(file.read_text().replace('  Shinogi','  dist/Shinogi'))
        with self.assertRaisesRegex(ValueError,'basename'):assets.validate(self.folder,'beta15')

if __name__=='__main__':unittest.main()

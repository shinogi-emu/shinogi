import hashlib
import importlib.util
from pathlib import Path
import plistlib
import tempfile
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
            for suffix in ['win64-setup.exe','macos-arm64.zip']:
                file=self.folder/f'{edition}-beta15-{suffix}'
                if suffix.endswith('.exe'):file.write_bytes(b'MZfixture')
                else:
                    with zipfile.ZipFile(file,'w') as archive:
                        archive.writestr(f'{edition}.app/Contents/Info.plist', plistlib.dumps({'CFBundleDisplayName':edition,'CFBundleIdentifier':'no.shinogi.emulator.'+cpu}))
                file.with_name(file.name+'.sha256').write_text(hashlib.sha256(file.read_bytes()).hexdigest()+'  '+file.name+'\n')
    def tearDown(self):self.tmp.cleanup()
    def test_exact_public_set(self):self.assertEqual(len(assets.validate(self.folder,'beta15')),8)
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

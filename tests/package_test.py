"""Validate the actual distributable, including its embedded source and hashes."""
from pathlib import Path
import hashlib
import io
import json
import unittest
import zipfile

ROOT=Path(__file__).resolve().parents[1]
VERSION=(ROOT/'VERSION').read_text().strip()
ARCHIVE=ROOT/f'release/OBS-Racing-Motion-{VERSION}-Windows-x64.zip'

class PackageTest(unittest.TestCase):
    def test_complete_portable_release(self):
        self.assertTrue(ARCHIVE.is_file(), 'versioned release ZIP is missing')
        with zipfile.ZipFile(ARCHIVE) as z:
            names=set(z.namelist())
            required={'START-HERE.txt','README.md','CHANGELOG.md','VERSION','LICENSE',
                      'Install-OBS.cmd','Install-ETS2.cmd','Install-ATS.cmd','install.ps1','install-game.ps1',
                      'THIRD-PARTY-NOTICES.txt','licenses/LLVM.txt','licenses/MinGW-runtime.txt',
                      'obs-racing-motion/bin/64bit/obs-racing-motion.dll',
                      'obs-racing-motion/data/motion.effect','obs-racing-motion/data/locale/ko-KR.ini',
                      'obs-racing-motion/data/locale/en-US.ini','truck/racing-motion-scs.dll',
                      'truck/SCS-SDK-LICENSE.txt',f'source/OBS-Racing-Motion-{VERSION}-Source.zip'}
            self.assertTrue(required<=names, required-names)
            self.assertEqual(z.read('VERSION').decode().strip(),VERSION)
            manifest=json.loads(z.read('SHA256SUMS.json'))
            self.assertEqual(set(manifest),names-{'SHA256SUMS.json'})
            for name,digest in manifest.items():
                self.assertNotIn('\\',name)
                self.assertNotIn('..',Path(name).parts)
                self.assertEqual(hashlib.sha256(z.read(name)).hexdigest(),digest,name)
            for name in ('README.md','START-HERE.txt','CHANGELOG.md'):
                content=z.read(name).decode('utf-8-sig')
                self.assertNotRegex(content,r'[A-Za-z]:[/\\](?:Users|Works)[/\\]')
            with zipfile.ZipFile(io.BytesIO(z.read(f'source/OBS-Racing-Motion-{VERSION}-Source.zip'))) as source:
                source_names=set(source.namelist())
                self.assertTrue({'src/plugin.cpp','src/motion.cpp','scripts/build.py','scripts/package.py',
                                 'scripts/bootstrap.py','truck/truck_telemetry.cpp','VERSION','LICENSE'}<=source_names)
                self.assertFalse(any(n.startswith(('.git/','.deps/','.tools/','build/','dist/','release/')) for n in source_names))
                self.assertIn(f'({VERSION})'.encode(),source.read('src/plugin.cpp'))
        digest=hashlib.sha256(ARCHIVE.read_bytes()).hexdigest()
        self.assertEqual(ARCHIVE.with_suffix('.zip.sha256').read_text().strip(),f'{digest}  {ARCHIVE.name}')

if __name__=='__main__':unittest.main(verbosity=2)

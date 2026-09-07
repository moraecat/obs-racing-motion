"""Exercise real installer copies, backups, WhatIf and integrity failures."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class InstallerTest(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='install-test-')
        self.base=Path(self.tmp.name)
        self.bundle=self.base/'bundle'
        bundle_source=Path(os.environ.get('RACING_INSTALL_TEST_BUNDLE',ROOT/'dist'))
        shutil.copytree(bundle_source,self.bundle)
        self.obs=self.base/'OBS plugins'
        self.game=self.base/'Euro Truck Simulator 2'
        (self.game/'bin/win_x64').mkdir(parents=True)
        (self.game/'bin/win_x64/eurotrucks2.exe').write_bytes(b'test fixture; not executable')
    def tearDown(self):
        self.tmp.cleanup()
    def install(self,what_if=False):
        env=os.environ.copy();env['LOCALAPPDATA']=str(self.base/'local')
        args=['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',str(self.bundle/'install.ps1'),
              '-ObsPluginRoot',str(self.obs),'-ETS2Path',str(self.game)]
        if what_if: args+=['-WhatIf']
        return subprocess.run(args,env=env,capture_output=True,text=True)
    def test_valid_copy_and_existing_file_backup(self):
        dll=self.game/'bin/win_x64/plugins/racing-motion-scs.dll'
        dll.parent.mkdir();dll.write_bytes(b'previous version')
        r=self.install();self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertEqual(dll.read_bytes(),(self.bundle/'truck/racing-motion-scs.dll').read_bytes())
        for p in (self.bundle/'obs-racing-motion').rglob('*'):
            if p.is_file():self.assertEqual(p.read_bytes(),(self.obs/'obs-racing-motion'/p.relative_to(self.bundle/'obs-racing-motion')).read_bytes())
        backups=list((self.base/'local').rglob('*racing-motion-scs.dll'))
        self.assertEqual(len(backups),1)
        self.assertEqual(backups[0].read_bytes(),b'previous version')
    def test_what_if_does_not_install(self):
        r=self.install(True);self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertFalse(self.obs.exists())
        self.assertFalse((self.game/'bin/win_x64/plugins').exists())
    def test_unlisted_dll_rejected_before_any_copy(self):
        (self.bundle/'obs-racing-motion/bin/64bit/unlisted.dll').write_bytes(b'extra')
        r=self.install();self.assertNotEqual(r.returncode,0)
        self.assertFalse(self.obs.exists())
    def test_missing_manifest_entry_rejected(self):
        path=self.bundle/'SHA256SUMS.json';manifest=json.loads(path.read_text())
        key=next(k for k in manifest if k.endswith('obs-racing-motion.dll'))
        del manifest[key];path.write_text(json.dumps(manifest))
        r=self.install();self.assertNotEqual(r.returncode,0)
        self.assertFalse(self.obs.exists())
    def test_corrupted_game_dll_rejected_before_any_copy(self):
        (self.bundle/'truck/racing-motion-scs.dll').write_bytes(b'corrupt')
        r=self.install();self.assertNotEqual(r.returncode,0)
        self.assertFalse(self.obs.exists())

if __name__=='__main__':unittest.main(verbosity=2)

"""Host-only validation of pinned release inputs."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import release

class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root/'app.bin').write_bytes(b'public synthetic app')
        (self.root/'cfg.bin').write_bytes(b'public synthetic cfg')
        self.manifest = {'product':'better-smc-mixer','install':{'app':'app.bin','cfg':'cfg.bin'},'files':{}}
        for name in ('app.bin','cfg.bin'):
            b=(self.root/name).read_bytes()
            self.manifest['files'][name]={'size':len(b),'sha256':hashlib.sha256(b).hexdigest()}
        self.path=self.root/'manifest.json'
        self.save()
    def save(self):
        self.path.write_text(json.dumps(self.manifest))
    def test_pin_binds_manifest_and_both_files(self):
        pin=release.validate(self.path)
        self.assertEqual(pin['manifest_sha256'],hashlib.sha256(self.path.read_bytes()).hexdigest())
        self.assertEqual(set(pin['install']),{'app','cfg'})
    def test_changed_bytes_refused(self):
        (self.root/'app.bin').write_bytes(b'wrong')
        with self.assertRaises(ValueError):release.validate(self.path)
    def test_traversal_refused_even_if_file_exists(self):
        self.manifest['install']['app']='../app.bin';self.save()
        with self.assertRaises(ValueError):release.validate(self.path)
    def test_symlink_refused(self):
        (self.root/'alias.bin').symlink_to(self.root/'app.bin')
        self.manifest['install']['app']='alias.bin';self.save()
        with self.assertRaises(ValueError):release.validate(self.path)
    def test_expected_build_is_bound_to_install_app(self):
        self.manifest['images']={'controller-ble':{'file':'app.bin','build_id':'a'*40,'sha256':self.manifest['files']['app.bin']['sha256'],'size':self.manifest['files']['app.bin']['size']}}
        self.save()
        self.assertEqual(release.validate(self.path)['expected_build'],'a'*40)
    def test_wrong_product_refused(self):
        self.manifest['product']='other';self.save()
        with self.assertRaises(ValueError):release.validate(self.path)

if __name__=='__main__':unittest.main()

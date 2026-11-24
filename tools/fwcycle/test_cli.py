"""CLI subprocess tests are host-only and never import the hardware adapter."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from evidence import Evidence
from model import initial

CLI = Path(__file__).with_name('fwcycle.py')


class CliTests(unittest.TestCase):
    def call(self, *args):
        return subprocess.run([sys.executable, '-I', str(CLI), *map(str, args)],
                              capture_output=True, text=True)

    def test_check_is_explicitly_host_only(self):
        result = self.call('check', '--host-only', '--json')
        self.assertEqual(result.returncode, 0, result.stderr)
        obj = json.loads(result.stdout)
        self.assertFalse(obj['hardware_tested'])
        self.assertFalse(obj['browser_attached'])

    def test_status_does_not_attach_and_ack_is_one_use(self):
        with tempfile.TemporaryDirectory(dir=Path(os.environ.get('TMPDIR', tempfile.gettempdir())).resolve()) as tmp:
            root = Path(tmp) / 'run'
            with Evidence.create(root, {}) as e:
                s = initial('usb', 'abc')
                s['state'] = 'await_power'
                s['checkpoint'] = {'id': 'pending', 'token': 'one-use', 'kind': 'power'}
                e.save_status(s)
            result = self.call('status', '--run', root, '--json')
            self.assertEqual(result.returncode, 3, result.stderr)
            self.assertEqual(json.loads(result.stdout)['state'], 'await_power')
            result = self.call('ack', '--run', root, '--checkpoint', 'pending', '--token', 'one-use')
            self.assertEqual(result.returncode, 3, result.stderr)
            result = self.call('ack', '--run', root, '--checkpoint', 'pending', '--token', 'one-use')
            self.assertEqual(result.returncode, 2)

    def test_status_and_stop_work_while_supervisor_holds_lock(self):
        with tempfile.TemporaryDirectory(dir=Path(os.environ.get('TMPDIR', tempfile.gettempdir())).resolve()) as tmp:
            root = Path(tmp) / 'run'
            with Evidence.create(root, {}) as e:
                s = initial('usb', 'abc')
                s['state'] = 'install_in_flight'
                s['effect'] = 'unknown'
                e.save_status(s)
                result = self.call('status', '--run', root, '--json')
                self.assertEqual(result.returncode, 3, result.stderr)
                result = self.call('stop', '--run', root, '--json')
                self.assertEqual(result.returncode, 3, result.stderr)
                self.assertTrue(e.read_json('stop-request.json')['requested'])
                self.assertEqual(e.read_status()['state'], 'install_in_flight')

    def test_plan_validates_local_release_without_device_access(self):
        with tempfile.TemporaryDirectory(dir=Path(os.environ.get('TMPDIR', tempfile.gettempdir())).resolve()) as tmp:
            root = Path(tmp)
            import hashlib
            manifest = {'product': 'better-smc-mixer', 'install': {'app': 'app.bin', 'cfg': 'cfg.bin'}, 'files': {}}
            for name in ('app.bin', 'cfg.bin'):
                data = b'synthetic release input'
                (root / name).write_bytes(data)
                manifest['files'][name] = {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
            path = root / 'manifest.json'
            path.write_text(json.dumps(manifest))
            result = self.call('plan', '--release', path, '--expected-build', 'abc', '--transport', 'usb', '--json')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)['transport'], 'usb')
            self.assertFalse(json.loads(result.stdout)['hardware_tested'])
            (root / 'app.bin').write_bytes(b'changed')
            self.assertEqual(self.call('plan', '--release', path, '--expected-build', 'abc').returncode, 2)

    def test_start_without_authorization_never_imports_browser(self):
        result = self.call('start', '--transport', 'ble', '--release', '/missing', '--run-dir', '/missing')
        self.assertEqual(result.returncode, 2)
        self.assertIn('allow-device', result.stderr)
        self.assertNotIn('bridge', result.stderr)

if __name__ == '__main__':
    unittest.main()

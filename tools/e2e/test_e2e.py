"""Tests of tools/e2e/e2e.py's logic without the mixer: the blocks option, the BLE merge rule, the
injection events, the panel's phase against build/host/devhost (its config port over SysEx). Run:
python3 -I tools/e2e/test_e2e.py
"""
import argparse
import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import e2e  # noqa: E402


class ConfigurationRestoreTest(unittest.TestCase):
    """The test runner must not save the owner's unsaved edits or lose a previous saved copy."""

    class Device:
        def __init__(self, running=b'working', stored=b'saved', generation=4):
            self.running, self.stored, self.generation = running, stored, generation
            self.writes = 0
            self.fail_stored_read = False
            self.pushes = []

        def info(self):
            return {'generation': self.generation, 'chunk': 128,
                    'store': 'clean' if self.running == self.stored else 'unsaved'}

        def pull(self, chunk):
            if self.fail_stored_read and self.running == self.stored:
                self.fail_stored_read = False
                raise e2e.devlink.DeviceError('stored read failed')
            return self.running

        def abort(self):
            pass

        def push(self, blob, chunk=128):
            self.pushes.append(blob)
            self.running = blob
            return self.info()

        def revert(self):
            self.running = self.stored
            return self.info()

        def save(self):
            if self.running != self.stored:
                self.stored = self.running
                self.generation += 1
                self.writes += 1
            return self.info()

    def test_unsaved_and_stored_copies_are_distinct(self):
        dev = self.Device()
        with tempfile.TemporaryDirectory() as out:
            backup = e2e.backup_configuration(dev, out)
            self.assertEqual(backup['running'], b'working')
            self.assertEqual(backup['stored'], b'saved')
            self.assertEqual(dev.running, b'working')
            self.assertEqual(dev.writes, 0)
            with open(os.path.join(out, 'before.bin'), 'rb') as f:
                self.assertEqual(f.read(), b'working')
            with open(os.path.join(out, 'stored-before.bin'), 'rb') as f:
                self.assertEqual(f.read(), b'saved')
            dev.push(b'test')
            dev.save()  # A phase can save, then raise before it returns a 'saved' flag.
            dev.push(b'other test edits')
            after = e2e.restore_configuration(dev, backup)
        self.assertEqual((dev.running, dev.stored), (b'working', b'saved'))
        self.assertEqual(after['store'], 'unsaved')
        self.assertEqual(dev.writes, 2)

    def test_no_test_save_needs_no_restore_write(self):
        dev = self.Device()
        with tempfile.TemporaryDirectory() as out:
            backup = e2e.backup_configuration(dev, out)
            dev.push(b'test')
            e2e.restore_configuration(dev, backup)
        self.assertEqual((dev.running, dev.stored, dev.writes), (b'working', b'saved', 0))

    def test_initially_clean_is_clean_after_restoration(self):
        dev = self.Device(running=b'saved')
        with tempfile.TemporaryDirectory() as out:
            backup = e2e.backup_configuration(dev, out)
            dev.push(b'test')
            dev.save()
            after = e2e.restore_configuration(dev, backup)
        self.assertEqual((dev.running, dev.stored), (b'saved', b'saved'))
        self.assertEqual(after['store'], 'clean')
        self.assertEqual(dev.writes, 2)

    def test_failed_stored_backup_restores_running_copy(self):
        dev = self.Device()
        dev.fail_stored_read = True
        with tempfile.TemporaryDirectory() as out:
            with self.assertRaisesRegex(e2e.devlink.DeviceError, 'stored read failed'):
                e2e.backup_configuration(dev, out)
        self.assertEqual((dev.running, dev.stored, dev.writes), (b'working', b'saved', 0))

    def test_failed_stored_verification_still_restores_running_copy(self):
        dev = self.Device()
        with tempfile.TemporaryDirectory() as out:
            backup = e2e.backup_configuration(dev, out)
            dev.push(b'test')
            dev.save()
            dev.fail_stored_read = True
            with self.assertRaisesRegex(e2e.devlink.DeviceError, 'stored read failed'):
                e2e.restore_configuration(dev, backup)
        self.assertEqual((dev.running, dev.stored), (b'working', b'saved'))

    def test_missing_or_unstable_baseline_refused_before_changes(self):
        dev = self.Device(generation=0)
        with tempfile.TemporaryDirectory() as out:
            with self.assertRaisesRegex(e2e.devlink.DeviceError, 'save a baseline'):
                e2e.backup_configuration(dev, out)
            self.assertEqual(dev.pushes, [])
            dev.info = lambda: {'generation': 4, 'store': 'writing', 'chunk': 128}
            with self.assertRaisesRegex(e2e.devlink.DeviceError, 'stable configuration'):
                e2e.backup_configuration(dev, out)
        self.assertEqual(dev.pushes, [])


class BlocksTest(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(e2e.parse_blocks("0:0,0:0s,3:3s"), [(0, 0, False), (0, 0, True), (3, 3, True)])


class InjectTest(unittest.TestCase):
    def test_events(self):
        self.assertEqual(e2e.inject_event("b 100 44 1".split()), bytes([1, 44, 1, 0, 0]))
        self.assertEqual(e2e.inject_event("f 100 2 16383".split()), bytes([3, 2, 0xFF, 0x3F, 0]))
        self.assertEqual(e2e.inject_event("e 100 7 -3".split()), bytes([2, 7, 0xFD, 0xFF, 0]))


class MergeTest(unittest.TestCase):
    """BLE's last value wins (FR-IO-08): only older values of continuous controls may be left out."""

    def test_unchanged(self):
        m = ["90 3C 7F", "B0 01 02", "80 3C 00"]
        self.assertIsNone(e2e.merged_away(m, list(m)))

    def test_older_values(self):
        logged = ["B0 1A 38", "B0 1A 40", "BF 06 38", "BF 26 00", "BF 06 30", "BF 26 00", "E1 00 40", "E1 7F 7F",
                  "D2 10", "D2 20"]
        self.assertIsNone(e2e.merged_away(logged, ["B0 1A 40", "BF 06 30", "BF 26 00", "E1 7F 7F", "D2 20"]))

    def test_refused(self):
        self.assertIn("90 3C 7F", e2e.merged_away(["90 3C 7F", "B0 01 02"], ["B0 01 02"]))  # a note
        self.assertIn("C0 05", e2e.merged_away(["C0 05", "C0 06"], ["C0 06"]))  # a program change
        self.assertIn("B0 01 01", e2e.merged_away(["B0 01 01", "B0 02 02"], ["B0 02 02"]))  # no newer value
        self.assertIn("B0 01 01", e2e.merged_away(["B0 01 01", "B0 01 02"], ["B0 01 02", "B0 01 01"]))  # reordered
        self.assertIn("did not send", e2e.merged_away(["B0 01 01"], ["B0 01 01", "B0 01 02"]))  # extra


DEVHOST = os.environ.get("SMC_DEVHOST") or os.path.join(e2e.ROOT, "build", "host", "devhost")


@unittest.skipUnless(os.access(DEVHOST, os.X_OK), "no build/host/devhost (make -C firmware sim)")
class PanelTest(unittest.TestCase):
    """Step 4 against the controller's portable code: the EVENTs, SELECT, touches by injection."""

    def test_panel_phase(self):
        host, dev = socket.socketpair()
        master, slave = os.openpty()
        proc = subprocess.Popen([DEVHOST, "30", "--sysex-fd", str(dev.fileno())], stdin=master, stdout=master,
                                pass_fds=(dev.fileno(),))
        dev.close()
        report = []
        try:
            time.sleep(0.1)
            a = argparse.Namespace(transport="sysex", config_port="fd:%d" % host.fileno(), port=None)
            e2e.panel_phase(a, lambda name, ok, detail="": report.append((name, ok, detail)))
        finally:
            proc.kill()
            proc.wait()
            host.close()
            os.close(master)
            os.close(slave)
        self.assertEqual(len(report), 6, report)
        self.assertTrue(all(ok for _, ok, _ in report), report)

    def test_saved_and_unsaved_restoration_against_devhost(self):
        host, device = socket.socketpair()
        with tempfile.TemporaryDirectory() as out:
            proc = subprocess.Popen([DEVHOST, '30', '--sysex-fd', str(device.fileno()),
                                     '--flash', os.path.join(out, 'flash.bin')],
                                    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                    pass_fds=(device.fileno(),))
            device.close()
            try:
                time.sleep(0.1)
                with e2e.devlink.Device(midi='fd:%d' % host.fileno()) as dev:
                    with open(os.path.join(e2e.ROOT, 'config/factory.json'), encoding='utf8') as f:
                        model = e2e.codec.from_json(e2e.json.load(f))
                    stored = e2e.codec.encode(model)
                    dev.push(stored)
                    dev.abort()
                    dev.save()
                    model['presets'][0]['channel'] = 4
                    original = e2e.codec.encode(model)
                    dev.push(original)
                    dev.abort()
                    backup = e2e.backup_configuration(dev, out)
                    self.assertEqual(backup['info']['store'], 'unsaved')
                    stored_after_revert = e2e.codec.decode(backup['stored'])
                    self.assertEqual(stored_after_revert, dict(e2e.codec.decode(stored), revision=3))
                    model['presets'][0]['channel'] = 5
                    dev.push(e2e.codec.encode(model))
                    dev.abort()
                    dev.save()  # No phase return flag: restoration checks the actual generation.
                    after = e2e.restore_configuration(dev, backup)
                    self.assertEqual(after['store'], 'unsaved')
                    self.assertEqual(e2e.codec.decode(dev.pull(128)),
                                     dict(e2e.codec.decode(original), revision=8))
                    dev.abort()
                    dev.revert()
                    self.assertEqual(e2e.codec.decode(dev.pull(128)),
                                     dict(e2e.codec.decode(stored), revision=9))
                    dev.abort()
            finally:
                proc.kill()
                proc.wait()
                proc.stdin.close()
                proc.stderr.close()
                host.close()


if __name__ == "__main__":
    unittest.main()

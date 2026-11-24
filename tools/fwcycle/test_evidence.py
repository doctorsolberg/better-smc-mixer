"""Private synthetic evidence and lock tests."""
import json
import os
from pathlib import Path
import tempfile
import unittest
from evidence import Evidence, EvidenceError


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=Path(os.environ.get('TMPDIR', tempfile.gettempdir())).resolve())
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / 'run'

    def test_private_atomic_status_and_durable_journal(self):
        with Evidence.create(self.root, {'transport': 'usb'}) as e:
            e.append('intent', {'action': 'rollback'})
            e.save_status({'state': 'rollback_in_flight'})
            self.assertEqual(e.read_status()['state'], 'rollback_in_flight')
            self.assertEqual(e.records()[0]['seq'], 1)
            self.assertEqual(self.root.stat().st_mode & 0o777, 0o700)
            for name in ('run.json', 'status.json', 'journal.jsonl', '.lock'):
                self.assertEqual((self.root / name).stat().st_mode & 0o777, 0o600)
        with Evidence.open(self.root) as e:
            e.append('observation', {})
            self.assertEqual([x['seq'] for x in e.records()], [1, 2])

    def test_second_owner_refused_and_dead_process_lock_released(self):
        with Evidence.create(self.root, {}) as e:
            with self.assertRaises(EvidenceError):
                Evidence.open(self.root)
        with Evidence.open(self.root) as e:
            self.assertEqual(e.records(), [])

    def test_symlink_directory_and_files_are_refused(self):
        target = Path(self.tmp.name) / 'target'
        target.mkdir(mode=0o700)
        self.root.symlink_to(target, target_is_directory=True)
        with self.assertRaises(EvidenceError):
            Evidence.open(self.root)
        self.root.unlink()
        with Evidence.create(self.root, {}) as e:
            (self.root / 'status.json').symlink_to(target / 'secret')
            with self.assertRaises(EvidenceError):
                e.save_status({'state': 'failed'})
            with self.assertRaises(EvidenceError):
                e.write_bytes('../escape', b'no')

    def test_torn_journal_is_not_silently_truncated(self):
        with Evidence.create(self.root, {}) as e:
            e.append('intent', {'action': 'install'})
        with (self.root / 'journal.jsonl').open('ab') as out:
            out.write(b'{"seq":2')
        with self.assertRaises(EvidenceError):
            Evidence.open(self.root)

    def test_different_run_directories_share_project_writer_lock(self):
        lock = Path(self.tmp.name) / 'project.lock'
        from evidence import project_lock
        with project_lock(lock):
            with self.assertRaises(EvidenceError):
                with project_lock(lock):
                    self.fail('second project writer acquired lock')
        with project_lock(lock):
            pass

    def test_attempt_directories_never_reused(self):
        with Evidence.create(self.root, {}) as e:
            first, second = e.new_attempt(), e.new_attempt()
            self.assertNotEqual(first, second)
            self.assertEqual(first.name, '0001')
            self.assertEqual(second.name, '0002')
            self.assertEqual(first.stat().st_mode & 0o777, 0o700)

if __name__ == '__main__':
    unittest.main()

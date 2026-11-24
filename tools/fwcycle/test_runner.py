"""Injected host-only cycle scenarios; no browser or hardware access."""
import copy
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from evidence import Evidence
from model import initial
from test_model import own
from runner import Runner

ROOT = Path(__file__).resolve().parents[2]


class Adapter:
    def __init__(self, transport, export):
        self.transport, self.export = transport, export
        self.phase = 'ours'
        self.actions = []
        self.restart = False

    def release(self, pin):
        return {'matched': True}

    def export_config(self, name):
        return self.export

    def snapshot(self):
        if self.phase == 'ours':
            s = own(self.transport)
            s['restart'] = {'verified': self.restart,'binding':{'pageId':'test-page','jobId':1}}
            return s
        s = {'initialized': True, 'identity': {'connected': True, 'kind': 'stock',
             'transport': self.transport.upper()}, 'stock': {'verified': True}}
        if self.phase == 'stock':
            s['rollback'] = {'completed': True}
        if self.phase in ('backup', 'installed'):
            s['job']={'stage':'backup_waiting' if self.phase=='backup' else 'complete','completed':self.phase=='installed'}
            s['backup'] = {'complete': True, 'path': str(self.export.parent / 'backup.bin'),
                           'plan_ok': True, 'release_match': True,'binding':{'pageId':'test-page','jobId':1}}
        if self.phase == 'installed':
            s['install'] = {'completed': True, 'readback_verified': True,
                            'boot_selection_verified': True,'binding':{'pageId':'test-page','jobId':1},'completion_sequence':20}
        return s

    def action(self, name, transport, **params):
        self.actions.append(name)
        self.phase = {'rollback': 'stock', 'backup': 'backup', 'install': 'installed',
                      'oursConnect': 'ours', 'stockConnect': 'stock'}.get(name, self.phase)
        if name == 'oursConnect':
            self.restart = True
        return {'accepted': True}


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=Path(os.environ.get('TMPDIR', tempfile.gettempdir())).resolve())
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)
        self.export = self.path / 'config.smcconf'
        self.export.write_bytes((ROOT / 'config/test.json').read_bytes())
        (self.path / 'backup.bin').write_bytes(bytes(524288))

    def new(self, transport='ble'):
        e = Evidence.create(self.path / 'run', {'release': {}})
        self.addCleanup(e.close)
        a = Adapter(transport, self.export)
        r = Runner(e, a, pin={}, state=initial(transport, 'abc123'))
        return r, a, e

    def test_usb_and_ble_cycles_pause_for_power_then_verify_exact_binary(self):
        for transport in ('usb', 'ble'):
            with self.subTest(transport=transport):
                if (self.path / 'run').exists():
                    import shutil
                    shutil.rmtree(self.path / 'run')
                r, a, e = self.new(transport)
                s = r.start(allow_device=True, allow_flash=True)
                self.assertEqual(s['state'], 'await_power')
                self.assertEqual(a.actions, ['rollback', 'backup', 'install'])
                checkpoint = s['checkpoint']
                r.ack(checkpoint['id'], checkpoint['token'])
                s = r.resume(allow_device=True, allow_flash=True)
                self.assertEqual(s['state'], 'passed_assisted')
                self.assertTrue(s['assertions']['configuration_exact'])
                self.assertEqual((e.root / 'before.bin').read_bytes(),
                                 (e.root / 'final.bin').read_bytes())
                self.assertEqual(a.actions.count('rollback'), 1)
                self.assertEqual(a.actions.count('install'), 1)
                e.close()

    def test_missing_consent_fails_before_any_adapter_action(self):
        r, a, e = self.new()
        with self.assertRaises(ValueError):
            r.start(allow_device=True)
        self.assertEqual(a.actions, [])

    def test_uncertain_install_can_reconcile_positive_completion_without_replay(self):
        r,a,e=self.new();s=r.start(True,True)
        s.update(state='needs_reconciliation',uncertain_action='install',monitor_only=True,effect='unknown');s.pop('checkpoint',None)
        r.authorized=True
        self.assertEqual(r.reconcile_install()['state'],'await_power')
        self.assertEqual(a.actions.count('install'),1)
    def test_lost_rollback_reply_never_replays_on_resume(self):
        r, a, e = self.new()
        original = a.action
        def lost(name, **params):
            original(name, **params)
            if name == 'rollback':
                raise TimeoutError('lost response')
        a.action = lost
        self.assertEqual(r.start(True, True)['state'], 'needs_reconciliation')
        recovered = Runner(e, a, pin={})
        self.assertEqual(recovered.resume(True, True)['state'], 'needs_reconciliation')
        self.assertEqual(a.actions, ['rollback'])

    def test_durable_intent_precedes_action(self):
        r, a, e = self.new()
        original = a.action
        def inspect(name, **params):
            if name in ('rollback', 'install'):
                self.assertTrue(any(x['kind'] == 'intent' and x['data']['action'] == name
                                    for x in e.records()))
                self.assertEqual(e.read_status()['state'], name + '_in_flight')
            return original(name, **params)
        a.action = inspect
        self.assertEqual(r.start(True, True)['state'], 'await_power')

    def test_disk_failure_before_intent_prevents_flash(self):
        r, a, e = self.new()
        original = e.append
        def full(kind, data):
            if kind == 'intent':
                raise OSError('disk full')
            return original(kind, data)
        e.append = full
        self.assertEqual(r.start(True, True)['state'], 'failed')
        self.assertEqual(a.actions, [])

    def test_wrong_or_reused_checkpoint_is_rejected(self):
        r, a, e = self.new()
        s = r.start(True, True)
        checkpoint = s['checkpoint']
        with self.assertRaises(ValueError):
            r.ack(checkpoint['id'], 'wrong')
        r.ack(checkpoint['id'], checkpoint['token'])
        with self.assertRaises(ValueError):
            r.ack(checkpoint['id'], checkpoint['token'])

    def test_stop_during_write_is_monitor_only(self):
        r, a, e = self.new()
        original = a.action
        def stop(name, **params):
            result = original(name, **params)
            if name == 'rollback':
                r.stop()
            return result
        a.action = stop
        s = r.start(True, True)
        self.assertEqual(a.actions, ['rollback'])
        self.assertTrue(s['monitor_only'])
        self.assertNotIn('disconnect', a.actions)

    def test_fsync_failure_before_dispatch_never_replays_after_restart(self):
        r, a, e = self.new()
        original = e.save_status
        def fail(state):
            if state['state'] == 'rollback_in_flight':
                raise OSError('fsync unavailable')
            return original(state)
        e.save_status = fail
        s = r.start(True, True)
        self.assertEqual(s['state'], 'needs_reconciliation')
        self.assertEqual(a.actions, [])
        recovered = Runner(e, a, {})
        recovered.resume(True, True)
        self.assertEqual(a.actions, [])

    def test_preexisting_backup_is_not_accepted_as_fresh(self):
        r, a, e = self.new()
        original = a.snapshot
        def stale():
            snapshot = original()
            if a.phase == 'stock':
                snapshot['backup'] = {'complete': True, 'path': str(self.path / 'backup.bin'),
                                      'plan_ok': True, 'release_match': True,'binding':{'pageId':'test-page','jobId':1}}
            return snapshot
        a.snapshot = stale
        self.assertEqual(r.start(True, True)['state'], 'await_power')
        self.assertIn('backup', a.actions)

    def test_resume_without_renewed_consent_does_not_observe_device(self):
        r, a, e = self.new()
        s = r.start(True, True)
        checkpoint = s['checkpoint']
        r.ack(checkpoint['id'], checkpoint['token'])
        def forbidden():
            self.fail('device observed without renewed authorization')
        a.snapshot = forbidden
        with self.assertRaises(ValueError):
            r.resume()

    def test_restarted_supervisor_does_not_continue_after_inflight_intent(self):
        r, a, e = self.new()
        s = initial('ble', 'abc123')
        s.update(state='install_in_flight', effect='unknown', intents=['install'])
        e.append('intent', {'action': 'install'})
        e.save_status(s)
        a.phase = 'installed'
        restored = Runner(e, a, {})
        self.assertEqual(restored.resume(True, True)['state'], 'needs_reconciliation')
        self.assertEqual(a.actions, [])

    def test_lost_install_reply_never_replays_or_power_prompts(self):
        r, a, e = self.new()
        original = a.action
        def lost(name, transport, **params):
            result = original(name, transport, **params)
            if name == 'install':
                raise TimeoutError('lost install reply')
            return result
        a.action = lost
        s = r.start(True, True)
        self.assertEqual(s['state'], 'needs_reconciliation')
        self.assertNotIn('checkpoint', s)
        Runner(e, a, {}).resume(True, True)
        self.assertEqual(a.actions.count('install'), 1)

    def test_unverified_readback_cannot_prompt_for_power(self):
        r, a, e = self.new()
        original = a.snapshot
        def incomplete():
            s = original()
            if 'install' in s:
                s['install']['readback_verified'] = False
            return s
        a.snapshot = incomplete
        s = r.start(True, True)
        self.assertEqual(s['state'], 'needs_reconciliation')
        self.assertNotIn('checkpoint', s)

    def test_ack_with_unchanged_restart_observation_cannot_pass(self):
        r, a, e = self.new()
        s = r.start(True, True)
        checkpoint = s['checkpoint']
        r.ack(checkpoint['id'], checkpoint['token'])
        a.phase = 'ours'
        a.restart = False
        self.assertEqual(r.resume(True, True)['state'], 'failed')

    def test_runner_creates_unique_attempt_on_reentry(self):
        r, a, e = self.new()
        r.start(True, True)
        Runner(e, a, {})
        self.assertEqual(sorted(p.name for p in (e.root / 'attempts').iterdir()), ['0001', '0002'])

    def test_changed_revision_fails_real_binary_comparison(self):
        r, a, e = self.new()
        s = r.start(True, True)
        raw = json.loads(self.export.read_text())
        raw['revision'] = raw.get('revision', 0) + 1
        self.export.write_text(json.dumps(raw))
        checkpoint = s['checkpoint']
        r.ack(checkpoint['id'], checkpoint['token'])
        self.assertEqual(r.resume(True, True)['state'], 'failed')
        self.assertFalse((e.root / 'result.json').exists())

    def test_revision_from_a_restart_passes_on_content_with_clean_storage(self):
        r, a, e = self.new()
        old_snapshot = a.snapshot
        def snapshot():
            s = old_snapshot()
            if s['identity'].get('kind') == 'ours':
                s['identity']['storage'] = 'clean'
            return s
        a.snapshot = snapshot
        s = r.start(True, True)
        raw = json.loads(self.export.read_text())
        raw['revision'] = raw.get('revision', 0) + 1
        self.export.write_text(json.dumps(raw))
        checkpoint = s['checkpoint']
        r.ack(checkpoint['id'], checkpoint['token'])
        s = r.resume(True, True)
        self.assertEqual(s['state'], 'passed_assisted')
        self.assertTrue(s['assertions']['configuration_content_exact'])
        self.assertFalse(s['assertions']['configuration_exact'])
        self.assertNotEqual((e.root / 'before.bin').read_bytes(), (e.root / 'final.bin').read_bytes())

    def test_content_change_with_clean_storage_fails(self):
        r, a, e = self.new()
        old_snapshot = a.snapshot
        def snapshot():
            s = old_snapshot()
            if s['identity'].get('kind') == 'ours':
                s['identity']['storage'] = 'clean'
            return s
        a.snapshot = snapshot
        s = r.start(True, True)
        raw = json.loads(self.export.read_text())
        raw['presets'][0]['pages'][0]['strips'][0]['fader']['number'] = 21
        self.export.write_text(json.dumps(raw))
        checkpoint = s['checkpoint']
        r.ack(checkpoint['id'], checkpoint['token'])
        s = r.resume(True, True)
        self.assertEqual(s['state'], 'failed')
        self.assertIn('content', s['reason'])

    def test_stop_mailbox_at_dispatch_boundary_prevents_write(self):
        r, a, e = self.new()
        original = e.save_status
        def stop(value):
            result = original(value)
            if value['state'] == 'rollback_in_flight':
                e.write_json('stop-request.json', {'requested': True})
            return result
        e.save_status = stop
        r.start(True, True)
        self.assertEqual(a.actions, [])

    def test_pending_stop_file_prevents_new_action(self):
        r, a, e = self.new()
        e.write_json('stop-request.json', {'requested': True})
        s = r.start(True, True)
        self.assertEqual(s['state'], 'failed')
        self.assertEqual(a.actions, [])

if __name__ == '__main__':
    unittest.main()

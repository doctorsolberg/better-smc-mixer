"""Host-only safety traces, never device tests."""
import copy
import unittest
from model import initial, transition, GuardError


def own(transport='ble', build='abc123'):
    return {'initialized': True, 'identity': {'connected': True, 'kind': 'ours',
            'transport': transport.upper(), 'build': build, 'version': '0.1.0',
            'protocol': 4, 'binaryForm': 1, 'schema': 1},
            'restart':{'binding':{'pageId':'test-page','jobId':1}},
            'config': {'saved': True, 'dirty': False, 'pulled': True},
            'rollback': {'ready': True}, 'panel': {'preset':0,'page':0,'shift':False,'bluetooth':True}, 'panelObservation':{'fresh':True}}


class CycleTests(unittest.TestCase):
    def test_both_transports_complete_assisted_trace(self):
        for transport in ('usb', 'ble'):
            with self.subTest(transport=transport):
                s = initial(transport, 'abc123')
                s = transition(s, 'authorized', device=True, flash=True)
                s = transition(s, 'baseline', snapshot=own(transport), digest='before')
                s = transition(s, 'arm_rollback')
                s = transition(s, 'rollback_intent')
                s = transition(s, 'rollback_complete', completed=True)
                stock = {'initialized': True, 'identity': {'connected': True,
                         'kind': 'stock', 'transport': transport.upper()},
                         'stock': {'verified': True}}
                s = transition(s, 'stock_connected', snapshot=stock)
                s = transition(s, 'backup_complete', complete=True, size=524288,
                               sha256='hash', plan_ok=True, release_match=True,binding={'pageId':'test-page','jobId':1})
                s = transition(s, 'install_intent')
                s = transition(s, 'install_complete', completed=True,
                               readback_verified=True, boot_selection_verified=True,binding={'pageId':'test-page','jobId':1},completion_sequence=20)
                self.assertEqual(s['state'], 'await_power')
                s = transition(s, 'power_ack')
                s = transition(s, 'ours_connected', snapshot=own(transport), restarted=True)
                s = transition(s, 'verify', snapshot=own(transport), digest='before')
                self.assertEqual(s['state'], 'passed_assisted')

    def test_unsaved_baseline_is_rejected(self):
        s = transition(initial('usb', 'abc123'), 'authorized', device=True, flash=True)
        snapshot = own('usb')
        snapshot['config']['dirty'] = True
        with self.assertRaisesRegex(GuardError, 'saved and clean'):
            transition(s, 'baseline', snapshot=snapshot, digest='before')

    def test_backup_size_alone_cannot_arm_install(self):
        s = initial('ble', 'abc123')
        s['state'] = 'backup'
        with self.assertRaises(GuardError):
            transition(s, 'backup_complete', complete=True, size=524288, sha256='hash')

    def test_final_identity_contract_cannot_drift(self):
        s = initial('ble', 'abc123')
        s['state'] = 'verify'
        s['baseline'] = {'snapshot': own(), 'digest': 'before'}
        for field in ('version', 'protocol', 'binaryForm', 'schema'):
            snapshot = own()
            snapshot['identity'][field] = 'changed'
            with self.subTest(field=field), self.assertRaises(GuardError):
                transition(s, 'verify', snapshot=snapshot, digest='before')

    def test_power_ack_does_not_prove_restart(self):
        s = initial('ble', 'abc123')
        s['state'] = 'await_power'
        s = transition(s, 'power_ack')
        with self.assertRaises(GuardError):
            transition(s, 'ours_connected', snapshot=own(), restarted=False)

    def test_revision_only_binary_change_cannot_pass(self):
        s = initial('ble', 'abc123')
        s['state'] = 'verify'
        s['baseline'] = {'snapshot': own(), 'digest': 'revision6'}
        with self.assertRaises(GuardError):
            transition(s, 'verify', snapshot=own(), digest='revision7')

    # P-6 and owner decision A (2026-11-23): the mixer assigns revisions and a restart boots the
    # saved one, so a revision differing alone passes only when the restarted mixer runs its saved
    # configuration (storage clean); the content must be byte-identical.
    def verify_state(self, content='content'):
        s = initial('ble', 'abc123')
        s['state'] = 'verify'
        s['baseline'] = {'snapshot': own(), 'digest': 'revision591', 'content_digest': content,
                         'revision': 591}
        return s

    def clean(self, storage='clean'):
        snap = own()
        snap['identity']['storage'] = storage
        snap['config'] = {'clean': storage == 'clean', 'saved': True, 'dirty': storage != 'clean',
                          'pulled': True}
        return snap

    def test_revision_alone_differs_after_restart_passes_with_content_exact(self):
        s = transition(self.verify_state(), 'verify', snapshot=self.clean(), digest='revision589',
                       content_digest='content', revision=589)
        self.assertEqual(s['state'], 'passed_assisted')
        self.assertTrue(s['assertions']['configuration_content_exact'])
        self.assertFalse(s['assertions']['configuration_exact'])
        self.assertEqual(s['assertions']['revision'], {'before': 591, 'after': 589,
                                                       'after_is_saved': True})

    def test_revision_alone_differs_without_clean_storage_fails(self):
        with self.assertRaisesRegex(GuardError, 'saved'):
            transition(self.verify_state(), 'verify', snapshot=self.clean('unsaved'),
                       digest='revision589', content_digest='content', revision=589)

    def test_content_change_fails_whatever_the_revision(self):
        with self.assertRaisesRegex(GuardError, 'content'):
            transition(self.verify_state(), 'verify', snapshot=self.clean(), digest='revision589',
                       content_digest='other', revision=589)

    def test_identical_binary_still_reports_exact(self):
        s = transition(self.verify_state(), 'verify', snapshot=self.clean(), digest='revision591',
                       content_digest='content', revision=591)
        self.assertTrue(s['assertions']['configuration_exact'])
        self.assertTrue(s['assertions']['configuration_content_exact'])

    def test_intent_cannot_be_reissued(self):
        s = initial('ble', 'abc123')
        s.update(state='install_ready', consent={'device': True, 'flash': True}, intents=['install'])
        with self.assertRaises(GuardError):
            transition(s, 'install_intent')

    def test_wrong_transport_cannot_establish_baseline(self):
        s = transition(initial('ble', 'abc123'), 'authorized', device=True, flash=True)
        with self.assertRaises(GuardError):
            transition(s, 'baseline', snapshot=own('usb'), digest='before')
        self.assertEqual(s['state'], 'baseline')

if __name__ == '__main__':
    unittest.main()

"""Page/job evidence remains exact across adapter dispatch and restart proof."""
import copy
from pathlib import Path
import tempfile
import unittest
from bridge import BrowserAdapter
from model import GuardError
from test_adapter import FakeRPC


class BridgeBinding(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.rpc = FakeRPC()
        self.adapter = BrowserAdapter('unused', 'unused', Path(self.tmp.name), rpc=self.rpc)
        self.binding = {'pageId': 'page-a', 'jobId': 7}

    def test_restart_uses_validated_install_completion_not_stale_latest(self):
        self.adapter.power_acknowledged = True
        self.rpc.state.update(job={'completed': True},
                              install={'completed': True, 'completion_sequence': 30,
                                       'binding': self.binding},
                              latest={'complete': {'sequence': 10}, 'connection': {'sequence': 20}})
        self.assertFalse(self.adapter.snapshot()['restart']['verified'])
        self.rpc.state['latest']['connection']['sequence'] = 31
        restart = self.adapter.snapshot()['restart']
        self.assertTrue(restart['verified'])
        self.assertEqual(restart['binding'], self.binding)


    def test_install_forwards_expected_binding_to_both_ui_steps(self):
        self.rpc.state['backup'] = {'binding': copy.deepcopy(self.binding)}
        self.adapter.action('install', 'usb', binding=copy.deepcopy(self.binding))
        actions = [params for method, params in self.rpc.calls if method == 'action']
        self.assertEqual(actions, [
            {'name': 'backup_next', 'transport': 'USB', 'binding': self.binding},
            {'name': 'install', 'transport': 'USB', 'binding': self.binding},
        ])

    def test_install_refuses_changed_binding_before_first_ui_step(self):
        self.rpc.state['backup'] = {'binding': {'pageId': 'other', 'jobId': 8}}
        with self.assertRaisesRegex(GuardError, 'binding'):
            self.adapter.action('install', 'usb', binding=self.binding)
        self.assertFalse(any(method == 'action' for method, _ in self.rpc.calls))


    def test_restart_requires_validated_install_even_with_completed_job(self):
        self.adapter.power_acknowledged = True
        self.rpc.state.update(job={'completed': True},
                              install={'completed': False, 'completion_sequence': 30,
                                       'binding': self.binding},
                              latest={'complete': {'sequence': 10}, 'connection': {'sequence': 40}})
        self.assertFalse(self.adapter.snapshot()['restart']['verified'])

    def test_install_keeps_binding_if_job_replaced_between_ui_steps(self):
        self.rpc.state['backup'] = {'binding': copy.deepcopy(self.binding)}
        original_call = self.rpc.call
        def call(method, params=None):
            result = original_call(method, params)
            if method == 'action' and params['name'] == 'backup_next':
                self.rpc.state['backup']['binding'] = {'pageId': 'other-page', 'jobId': 99}
            return result
        self.rpc.call = call
        self.adapter.action('install', 'usb', binding=copy.deepcopy(self.binding))
        install = [params for method, params in self.rpc.calls
                   if method == 'action' and params['name'] == 'install']
        self.assertEqual(install[0]['binding'], self.binding)


if __name__ == '__main__':
    unittest.main()

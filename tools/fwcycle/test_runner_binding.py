"""Runner preserves the validated page/job binding through the install boundary."""
import unittest
import test_runner


class RunnerBinding(unittest.TestCase):
    setUp = test_runner.RunnerTests.setUp
    new = test_runner.RunnerTests.new

    def test_backup_completion_persists_observed_binding(self):
        runner, adapter, evidence = self.new()
        runner.state.update(state='backup', backup_requested=True)
        adapter.phase = 'backup'
        runner.authorized = True
        result = runner.run(max_steps=1)
        self.assertEqual(result['state'], 'await_owner')
        self.assertEqual(result['resume_state'], 'install_ready')
        self.assertEqual(result['backup']['binding'], adapter.snapshot()['backup']['binding'])
        self.assertEqual(evidence.read_status()['backup']['binding'], result['backup']['binding'])


    def test_install_refuses_changed_or_nonwaiting_backup_job(self):
        for mutation in ('binding', 'stage', 'completed'):
            with self.subTest(mutation=mutation):
                import shutil
                if (self.path / 'run').exists():
                    shutil.rmtree(self.path / 'run')
                runner, adapter, evidence = self.new()
                adapter.phase = 'backup'
                observation = adapter.snapshot()
                runner.state.update(state='install_ready', backup=observation['backup'],
                                    consent={'device': True, 'flash': True})
                runner.authorized = True
                original_snapshot = adapter.snapshot
                def snapshot():
                    result = original_snapshot()
                    result['job'] = {'stage': 'backup_waiting', 'completed': False}
                    if mutation == 'binding':
                        result['backup']['binding'] = {'pageId': 'new-page', 'jobId': 99}
                    elif mutation == 'stage':
                        result['job']['stage'] = 'complete'
                    else:
                        result['job']['completed'] = True
                    return result
                adapter.snapshot = snapshot
                result = runner.run(max_steps=1)
                self.assertEqual(result['state'], 'failed')
                self.assertNotIn('install', adapter.actions)
                evidence.close()
                import shutil
                shutil.rmtree(evidence.root)


    def test_install_completion_uses_only_expected_binding(self):
        runner, adapter, evidence = self.new()
        adapter.phase = 'installed'
        observation = adapter.snapshot()
        runner.state.update(state='install_in_flight', backup=observation['backup'],
                            intents=['install'], effect='unknown')
        runner.authorized = True
        original_snapshot = adapter.snapshot
        def snapshot():
            result = original_snapshot()
            result['install']['binding'] = {'pageId': 'other-page', 'jobId': 2}
            return result
        adapter.snapshot = snapshot
        result = runner.run(max_steps=1)
        self.assertEqual(result['state'], 'needs_reconciliation')
        self.assertTrue(result['monitor_only'])
        self.assertNotIn('checkpoint', result)
        self.assertNotIn('install', adapter.actions)

    def test_install_completion_persists_positive_sequence(self):
        runner, adapter, evidence = self.new()
        adapter.phase = 'installed'
        observation = adapter.snapshot()
        runner.state.update(state='install_in_flight', backup=observation['backup'],
                            intents=['install'], effect='unknown')
        runner.authorized = True
        result = runner.run(max_steps=1)
        self.assertEqual(result['state'], 'await_power')
        self.assertEqual(evidence.read_status()['completion_sequence'],
                         observation['install']['completion_sequence'])


    def test_reconciliation_requires_expected_binding_and_sequence(self):
        from model import GuardError
        import shutil
        for mutation in ('binding', 'sequence', 'valid'):
            with self.subTest(mutation=mutation):
                if (self.path / 'run').exists():
                    shutil.rmtree(self.path / 'run')
                runner, adapter, evidence = self.new()
                adapter.phase = 'installed'
                observation = adapter.snapshot()
                runner.state.update(state='needs_reconciliation', backup=observation['backup'],
                                    intents=['install'], effect='unknown', monitor_only=True)
                runner.authorized = True
                original_snapshot = adapter.snapshot
                def snapshot():
                    result = original_snapshot()
                    if mutation == 'binding':
                        result['install']['binding'] = {'pageId': 'other-page', 'jobId': 2}
                    elif mutation == 'sequence':
                        result['install']['completion_sequence'] = 0
                    return result
                adapter.snapshot = snapshot
                if mutation == 'valid':
                    result = runner.reconcile_install()
                    self.assertEqual(result['state'], 'await_power')
                    self.assertEqual(result['completion_sequence'], observation['install']['completion_sequence'])
                else:
                    with self.assertRaisesRegex(GuardError, 'binding|sequence'):
                        runner.reconcile_install()
                    self.assertEqual(runner.state['state'], 'needs_reconciliation')
                self.assertEqual(adapter.actions, [])
                evidence.close()


    def test_install_dispatch_carries_persisted_backup_binding(self):
        runner, adapter, evidence = self.new()
        binding = {'pageId': 'test-page', 'jobId': 1}
        runner.state['backup'] = {'binding': binding}
        runner.authorized = True
        calls = []
        adapter.action = lambda name, **params: calls.append((name, params))
        runner.dispatch('install')
        self.assertEqual(calls, [('install', {'transport': 'ble', 'binding': binding})])


if __name__ == '__main__':
    unittest.main()

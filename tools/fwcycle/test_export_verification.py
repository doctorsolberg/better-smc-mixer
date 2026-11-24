"""Final guards must use observations taken after the export finishes."""
import unittest
import test_runner


class ExportVerification(unittest.TestCase):
    setUp = test_runner.RunnerTests.setUp
    new = test_runner.RunnerTests.new

    def test_panel_drift_during_final_export_cannot_pass(self):
        runner, adapter, evidence = self.new()
        from runner import encoded_config
        import hashlib
        baseline = adapter.snapshot()
        _, binary = encoded_config(self.export)
        runner.state.update(state='verify', baseline={
            'snapshot': baseline, 'digest': hashlib.sha256(binary).hexdigest()})
        runner.authorized = True
        original_export = adapter.export_config
        original_snapshot = adapter.snapshot
        drifted = [False]

        def export(name):
            if name == 'final':
                drifted[0] = True
            return original_export(name)

        def snapshot():
            observation = original_snapshot()
            if drifted[0]:
                observation['panel']['page'] = 3
            return observation

        adapter.export_config = export
        adapter.snapshot = snapshot
        result = runner.resume(True, True)
        self.assertEqual(result['state'], 'failed')
        self.assertIn('panel', result['reason'])
        self.assertFalse(evidence.path('result.json').exists())
        self.assertEqual(result['last_observation']['panel']['page'], 3)


if __name__ == '__main__':
    unittest.main()

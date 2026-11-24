"""Asynchronous stock connection must stay owned until connected or bounded stop."""
import unittest
import test_runner
class Connections(unittest.TestCase):
    setUp=test_runner.RunnerTests.setUp
    new=test_runner.RunnerTests.new
    def test_stock_connect_is_observed_without_early_checkpoint(self):
        r,a,e=self.new();old_snapshot=a.snapshot;old_action=a.action;pending=[0];connected=[False]
        def action(name,**params):
            result=old_action(name,**params)
            if name=='rollback':connected[0]=False
            if name=='stockConnect':pending[0]=2
            return result
        def snapshot():
            s=old_snapshot()
            if a.phase=='stock' and not connected[0]:
                if pending[0]:
                    pending[0]-=1
                    if not pending[0]:connected[0]=True
                s['identity']['kind']='ours';s['identity']['connected']=False
            return s
        a.action=action;a.snapshot=snapshot;r.poll_interval=0
        self.assertEqual(r.start(True,True)['state'],'await_power')
        self.assertEqual(a.actions.count('stockConnect'),1)
    def test_success_clears_resolved_reason_not_journal(self):
        r,a,e=self.new();s=r.start(True,True);cp=s['checkpoint'];r.ack(cp['id'],cp['token'])
        r.state['reason']='old transport problem';e.append('old_failure',{'reason':r.state['reason']})
        s=r.resume(True,True)
        self.assertEqual(s['state'],'passed_assisted');self.assertNotIn('reason',s)
        self.assertTrue(any(v['kind']=='old_failure' for v in e.records()))
    def test_stock_prerequisite_checkpoint_rechecks_only_after_ack(self):
        r, a, e = self.new()
        r.state.update(state='stock_connect', effect='rollback_complete',
                       consent={'device': True, 'flash': True})
        r.save()
        ready = [False]
        checks = []

        def readiness(name, transport):
            checks.append((name, transport))
            self.assertFalse(e.read_status().get('stock_connect_requested', False))
            return {'ready': ready[0], 'kind': 'native_connection', 'message': 'Connect exact endpoint.'}

        a.connection_readiness = readiness
        result = r.resume(True, True)
        self.assertEqual(result['state'], 'await_owner')
        self.assertFalse(result.get('stock_connect_requested', False))
        self.assertNotIn('stockConnect', a.actions)
        self.assertEqual(len(checks), 1)
        self.assertEqual(r.resume(True, True)['state'], 'await_owner')
        self.assertEqual(len(checks), 1)
        checkpoint = result['checkpoint']
        r.ack(checkpoint['id'], checkpoint['token'])
        ready[0] = True
        result = r.resume(True, True)
        self.assertEqual(result['state'], 'await_power')
        self.assertEqual(len(checks), 2)
        self.assertEqual(a.actions.count('stockConnect'), 1)
        self.assertTrue(e.read_status()['stock_connect_requested'])

    def test_dispatched_connect_never_replays_after_ack_and_restart(self):
        from runner import Runner
        r, a, e = self.new()
        r.state.update(state='ours_connect', effect='install_complete')
        a.phase = 'installed'
        a.connection_readiness = lambda name, transport: {'ready': True}
        checks = []
        def action(name, **params):
            self.assertTrue(e.read_status()['ours_connect_requested'])
            checks.append((name, params))
            return {'dispatched': True}
        a.action = action
        r.operation_timeout = 0
        result = r.resume(True, True)
        self.assertEqual(result['state'], 'await_owner')
        checkpoint = result['checkpoint']
        r.ack(checkpoint['id'], checkpoint['token'])
        recovered = Runner(e, a, {})
        recovered.operation_timeout = 0
        self.assertEqual(recovered.resume(True, True)['state'], 'await_owner')
        self.assertEqual(len(checks), 1)
        self.assertTrue(checks[0][1]['readiness_checked'])

    def test_nonprerequisite_guard_errors_stay_terminal(self):
        from model import GuardError
        r, a, e = self.new()
        r.state.update(state='ours_connect', effect='install_complete')
        a.phase = 'installed'
        def unsafe(name, transport):
            raise GuardError('identity/transport guard rejected')
        a.connection_readiness = unsafe
        result = r.resume(True, True)
        self.assertEqual(result['state'], 'failed')
        self.assertIn('identity/transport', result['reason'])
        self.assertNotIn('checkpoint', result)
        self.assertFalse(result.get('ours_connect_requested', False))
        self.assertEqual(a.actions, [])

    def test_ours_prerequisite_failure_does_not_mark_dispatch(self):
        r, a, e = self.new()
        r.state.update(state='ours_connect', effect='install_complete')
        a.phase = 'installed'
        a.connection_readiness = lambda name, transport: {
            'ready': False, 'kind': 'native_connection', 'message': 'Connect Better-SMC-Mixer.'}
        result = r.resume(True, True)
        self.assertEqual(result['state'], 'await_owner')
        self.assertEqual(result['resume_state'], 'ours_connect')
        self.assertFalse(e.read_status().get('ours_connect_requested', False))
        self.assertEqual(a.actions, [])

    def test_readiness_never_runs_without_current_authorization(self):
        r, a, e = self.new()
        r.state.update(state='ours_connect', effect='install_complete')
        a.phase = 'installed'
        checks = []
        a.connection_readiness = lambda name, transport: checks.append(name) or {'ready': True}
        result = r.run(max_steps=1)
        self.assertEqual(result['state'], 'failed')
        self.assertEqual(checks, [])
        self.assertEqual(a.actions, [])

    def test_connect_rpc_timeout_never_becomes_retryable_prerequisite(self):
        r, a, e = self.new()
        r.state.update(state='ours_connect', effect='install_complete')
        a.phase = 'installed'
        a.connection_readiness = lambda name, transport: {'ready': True}
        calls = []
        def lost(name, **params):
            calls.append(name)
            raise TimeoutError('RPC action outcome unknown')
        a.action = lost
        result = r.resume(True, True)
        self.assertEqual(result['state'], 'failed')
        self.assertTrue(e.read_status()['ours_connect_requested'])
        self.assertNotIn('checkpoint', result)
        r.resume(True, True)
        self.assertEqual(calls, ['oursConnect'])

if __name__=='__main__':unittest.main()

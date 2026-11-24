"""Review regressions use actual nullable and async observer shapes."""
from pathlib import Path
import tempfile
import unittest
from bridge import BrowserAdapter
from test_adapter import FakeRPC
import test_runner

class AdapterReview(unittest.TestCase):
    def setUp(self):
        self.t=tempfile.TemporaryDirectory();self.addCleanup(self.t.cleanup)
        self.rpc=FakeRPC();self.rpc.state['job']=None
        self.a=BrowserAdapter('http://127.0.0.1:1234','http://127.0.0.1:8876',Path(self.t.name),rpc=self.rpc)
        self.a.poll_interval=0
    def test_null_job_is_normal_preinstall_state(self):
        self.assertTrue(self.a.wait(lambda s:s['initialized'])['initialized'])
    def test_release_forwards_raw_manifest_hash(self):
        self.a.release({'manifest':{},'manifest_sha256':'a'*64})
        self.assertIn(('release',{'manifest':{},'manifest_sha256':'a'*64}),self.rpc.calls)
    def test_new_export_accepts_only_new_download_guid(self):
        p=self.a.downloads/'old';p.write_bytes(b'old')
        old_call=self.rpc.call;requested=[False]
        def call(method,params=None):
            if method=='exportConfig':requested[0]=True;(self.a.downloads/'new').write_bytes(b'new')
            if method=='downloads':return {'downloads':[{'guid':g,'name':'final.json','state':'completed'} for g in (['old','new'] if requested[0] else ['old'])]}
            return old_call(method,params)
        self.rpc.call=call
        self.assertEqual(self.a.export_config('final'),self.a.downloads/'new')
    def test_new_export_rejects_old_same_name_receipt(self):
        p=self.a.downloads/'old';p.write_bytes(b'old')
        old_call=self.rpc.call
        def call(method,params=None):
            if method=='downloads':return {'downloads':[{'guid':'old','name':'final.json','state':'completed'}]}
            return old_call(method,params)
        self.rpc.call=call
        self.a.deadline=.02
        with self.assertRaises(TimeoutError):self.a.export_config('final')
    def test_completed_receipt_survives_bridge_replacement(self):
        p=self.a.downloads/'one';p.write_bytes(b'fresh')
        original=self.rpc.call
        self.rpc.call=lambda method,params=None: {'downloads':[{'guid':'one','name':'before.json','state':'completed'}]} if method=='downloads' else original(method,params)
        self.assertEqual(self.a.completed_download(name='before.json'),p)
        fresh=BrowserAdapter('http://127.0.0.1:1234','http://127.0.0.1:8876',Path(self.t.name),rpc=FakeRPC())
        self.assertEqual(fresh.completed_download(name='before.json'),p)
        p.write_bytes(b'changed')
        self.assertIsNone(fresh.completed_download(name='before.json'))

class RunnerReview(unittest.TestCase):
    setUp=test_runner.RunnerTests.setUp
    new=test_runner.RunnerTests.new
    def test_delayed_reconnect_only_dispatches_once(self):
        r,a,e=self.new();s=r.start(True,True);cp=s['checkpoint'];r.ack(cp['id'],cp['token'])
        old_action=a.action;old_snapshot=a.snapshot;waiting=[0]
        def action(name,**kw):
            result=old_action(name,**kw)
            if name=='oursConnect':waiting[0]=2
            return result
        def snapshot():
            if waiting[0]:
                waiting[0]-=1
                return {'initialized':True,'identity':{'connected':False,'kind':'ours'}}
            return old_snapshot()
        a.action=action;a.snapshot=snapshot;r.poll_interval=0
        self.assertEqual(r.resume(True,True)['state'],'passed_assisted')
        self.assertEqual(a.actions.count('oursConnect'),1)
    def test_continuation_discards_stale_final_digest(self):
        import json
        r,a,e=self.new();s=r.start(True,True)
        s.update(state='verify',final_digest=s['baseline']['digest']);s.pop('checkpoint',None)
        a.phase='ours';a.restart=True
        raw=json.loads(self.export.read_text());raw['revision']=raw.get('revision',0)+1;self.export.write_text(json.dumps(raw))
        self.assertEqual(r.resume(True,True)['state'],'failed')
    def test_power_boundary_restored_on_reconnect_continuation(self):
        r,a,e=self.new();s=r.start(True,True);cp=s['checkpoint'];r.ack(cp['id'],cp['token'])
        a.power_acknowledged=False
        old_action=a.action
        def pause(name,**kw):
            result=old_action(name,**kw)
            if name=='oursConnect':raise RuntimeError('native help needed')
            return result
        # Persisted explicit boundary must be available to any later adapter.
        s.update(state='await_owner',resume_state='ours_connect',power_boundary_acknowledged=True);s.pop('checkpoint',None)
        a.phase='ours';a.restart=True
        r.resume(True,True)
        self.assertTrue(a.power_acknowledged)
    def test_safe_resume_rebinds_release_before_backup(self):
        r,a,e=self.new();s=r.start(True,True)
        s.update(state='await_owner',resume_state='backup');s.pop('checkpoint',None);s['backup_requested']=False;e.save_status(s)
        calls=[];a.release=lambda pin:calls.append('release') or {'matched':True}
        r.resume(True,True)
        self.assertIn('release',calls)

if __name__=='__main__':unittest.main()

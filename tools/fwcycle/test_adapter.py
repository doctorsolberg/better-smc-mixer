"""Python integration contract for the typed real-editor bridge, no device access."""
from pathlib import Path
import tempfile
import unittest
from bridge import BrowserAdapter

class FakeRPC:
    def __init__(self):self.calls=[];self.state={'initialized':True,'identity':{'connected':True,'kind':'ours','transport':'USB'},'restart':{'verified':False},'dom':{'dialogOpen':False},'config':{'pulled':True}}
    def call(self,method,params=None):
        self.calls.append((method,params))
        if method=='snapshot':return self.state
        if method=='release':return {'manifest':params['manifest']}
        if method=='action' and params.get('name') in {'baseline','final_export'}:
            old=self.state.get('panelObservation',{})
            self.state['panelObservation']={'fresh':True,'readSequence':old.get('readSequence',0)+1,'connectionEpoch':old.get('connectionEpoch')}
        if method=='downloads':return {'downloads':[]}
        return {'dispatched':True}

class AdapterTests(unittest.TestCase):
    def setUp(self):
        self.t=tempfile.TemporaryDirectory();self.addCleanup(self.t.cleanup)
        self.rpc=FakeRPC()
        self.adapter=BrowserAdapter('http://127.0.0.1:1234','http://127.0.0.1:8876',Path(self.t.name),rpc=self.rpc)
        self.adapter.poll_interval=0
    def test_action_transport_and_endpoint_are_exact(self):
        self.adapter.action('oursConnect','usb')
        self.assertIn(('action',{'name':'ours_connect','transport':'USB','endpoint':'SMC-Mixer Config'}),self.rpc.calls)
    def test_release_passes_actual_manifest_not_python_wrapper(self):
        self.assertEqual(self.adapter.release({'manifest':{'product':'better-smc-mixer'}}),{'matched':True})
    def test_completed_download_matches_bridge_name_and_content(self):
        p=self.adapter.downloads/'guid1';p.write_bytes(b'json-data')
        old=self.rpc.call
        def call(method,params=None):
            if method=='downloads':return {'downloads':[{'guid':'guid1','name':'before.json','state':'completed'}]}
            return old(method,params)
        self.rpc.call=call
        self.assertEqual(self.adapter.completed_download(name='before.json'),p)
        self.assertIsNone(self.adapter.completed_download(name='other.json'))
    def test_unknown_action_rejected(self):
        with self.assertRaises(ValueError):self.adapter.action('eval','ble')
    def test_restart_requires_completed_install_and_fresh_connection(self):
        self.adapter.power_acknowledged=True
        self.assertFalse(self.adapter.snapshot()['restart']['verified'])
        self.rpc.state['job']={'completed':True,'id':1}
        self.rpc.state['install']={'completed':True,'completion_sequence':10,'binding':{'pageId':'test-page','jobId':1}}
        self.rpc.state['latest']={'connection':{'sequence':20},'complete':{'sequence':10}}
        self.assertTrue(self.adapter.snapshot()['restart']['verified'])

if __name__=='__main__':unittest.main()

"""Test subprocess RPC with an isolated host-only child."""
from pathlib import Path
import sys
import unittest
from bridge import NodeBridge

class BridgeTests(unittest.TestCase):
    def test_roundtrip_and_child_error(self):
        script="import sys,json\nfor line in sys.stdin:\n r=json.loads(line);print(json.dumps({'id':r['id'],'result':r['method']}),flush=True)"
        with NodeBridge(command=[sys.executable,'-I','-c',script]) as b:
            self.assertEqual(b.call('hello'),'hello')
            self.assertEqual(b.call('status'),'status')
    def test_wrong_id_refused(self):
        script="import sys,json\nr=json.loads(input());print(json.dumps({'id':r['id']+1,'result':True}),flush=True)"
        with NodeBridge(command=[sys.executable,'-I','-c',script]) as b:
            with self.assertRaises(RuntimeError):b.call('hello')
    def test_timeout_does_not_assume_action_failed(self):
        script="import sys,time\ninput();time.sleep(1)"
        with NodeBridge(command=[sys.executable,'-I','-c',script],timeout=.05) as b:
            with self.assertRaises(TimeoutError):b.call('install')
if __name__=='__main__':unittest.main()

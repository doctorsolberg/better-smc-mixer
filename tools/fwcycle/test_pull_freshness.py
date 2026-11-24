import unittest,tempfile
from pathlib import Path
from bridge import BrowserAdapter
from test_adapter import FakeRPC
class FreshPull(unittest.TestCase):
 def test_old_pulled_flag_cannot_complete_new_pull(self):
  with tempfile.TemporaryDirectory() as t:
   rpc=FakeRPC();rpc.state['panelObservation']={'fresh':True,'readSequence':7,'connectionEpoch':2}
   rpc.state['panel']={'preset':0,'page':3,'shift':False,'bluetooth':'on'}
   a=BrowserAdapter('http://127.0.0.1:1','http://127.0.0.1:8876',Path(t),rpc=rpc)
   old_call=rpc.call
   rpc.call=lambda m,p=None: {'dispatched':True} if m=='action' else old_call(m,p)
   def wait(predicate,seconds=None):
    self.assertFalse(predicate(a.snapshot()),'stale read certified new pull')
    rpc.state['panelObservation']['readSequence']=8
    self.assertTrue(predicate(a.snapshot()))
    raise TimeoutError('test ends after fresh guard')
   a.wait=wait
   with self.assertRaisesRegex(TimeoutError,'test ends'):a.export_config('before')
if __name__=='__main__':unittest.main()

"""A fresh pull is performed before enforcing pulled-baseline evidence."""
import unittest
import test_runner

class BaselineTests(unittest.TestCase):
    setUp=test_runner.RunnerTests.setUp
    new=test_runner.RunnerTests.new
    def test_reconnected_final_export_has_only_one_download(self):
        r,a,e=self.new();s=r.start(True,True);cp=s['checkpoint'];r.ack(cp['id'],cp['token'])
        old=a.snapshot;old_export=a.export_config;exports=[]
        def snapshot():
            s=old()
            if a.restart:s['config']['pulled']=False
            return s
        def export(name):exports.append(name);return old_export(name)
        a.snapshot=snapshot;a.export_config=export
        r.resume(True,True)
        self.assertLessEqual(exports.count('final'),1)
    def test_reconnect_refreshes_pull_before_final_identity_guard(self):
        r,a,e=self.new()
        s=r.start(True,True)
        cp=s['checkpoint'];r.ack(cp['id'],cp['token'])
        old=a.snapshot;old_export=a.export_config
        pulled=[False]
        def snapshot():
            s=old()
            if a.restart:s['config']['pulled']=pulled[0]
            return s
        def export(name):
            if name=='final':pulled[0]=True
            return old_export(name)
        a.snapshot=snapshot;a.export_config=export
        self.assertEqual(r.resume(True,True)['state'],'passed_assisted')
    def test_baseline_export_refreshes_pull_before_guard(self):
        r,a,e=self.new()
        old=a.snapshot
        pulled=[False]
        def snapshot():
            s=old()
            if a.phase=='ours':s['config']['pulled']=pulled[0]
            return s
        old_export=a.export_config
        def export(name):pulled[0]=True;return old_export(name)
        a.snapshot=snapshot;a.export_config=export
        self.assertEqual(r.start(True,True)['state'],'await_power')
if __name__=='__main__':unittest.main()

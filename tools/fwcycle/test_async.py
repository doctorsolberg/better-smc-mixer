"""Host-only slow-operation observations must not be mistaken for failed writes."""
import unittest
import test_runner

class AsyncTests(unittest.TestCase):
    setUp=test_runner.RunnerTests.setUp
    new=test_runner.RunnerTests.new
    def test_pending_install_waits_for_real_completion(self):
        r,a,e=self.new()
        original=a.snapshot
        pending=[0]
        def snapshot():
            s=original()
            if a.phase=='installed' and pending[0]<2:
                pending[0]+=1
                s['install']={'active':True,'completed':False,'stage':'area','done':pending[0],'total':3}
            return s
        a.snapshot=snapshot
        r.poll_interval=0
        self.assertEqual(r.start(True,True)['state'],'await_power')
        self.assertEqual(a.actions.count('install'),1)

if __name__=='__main__':unittest.main()

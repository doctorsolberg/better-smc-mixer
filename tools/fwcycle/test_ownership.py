"""A live CLI cannot attach an unverified profile even with audit flags."""
import subprocess
import sys
from pathlib import Path
import unittest

class OwnershipTests(unittest.TestCase):
    def test_live_start_requires_profile_before_browser_attachment(self):
        script=Path(__file__).with_name('fwcycle.py')
        result=subprocess.run([sys.executable,'-I',str(script),'start','--transport','usb','--release','missing.json','--run-dir','missing-dir','--allow-device','--allow-flash','--endpoint','http://127.0.0.1:1','--origin','http://127.0.0.1:8876'],capture_output=True,text=True,timeout=5)
        self.assertEqual(result.returncode,2)
        self.assertIn('--profile',result.stderr)
if __name__=='__main__':unittest.main()

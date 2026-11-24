import unittest
from model import initial,transition,GuardError
class Binding(unittest.TestCase):
 def test_backup_requires_page_and_job(self):
  s=initial('usb','x');s['state']='backup'
  with self.assertRaisesRegex(GuardError,'binding'):
   transition(s,'backup_complete',complete=True,size=524288,sha256='h',plan_ok=True,release_match=True)
 def test_install_cannot_complete_from_another_job_or_page(self):
  s=initial('usb','x');s['state']='install_in_flight';s['backup']={'binding':{'pageId':'a','jobId':2}}
  for b in [{'pageId':'b','jobId':2},{'pageId':'a','jobId':1},None]:
   with self.subTest(b=b),self.assertRaisesRegex(GuardError,'binding'):
    transition(s,'install_complete',completed=True,readback_verified=True,boot_selection_verified=True,binding=b,completion_sequence=20)
if __name__=='__main__':unittest.main()

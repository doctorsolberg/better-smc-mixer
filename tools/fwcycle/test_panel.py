"""Panel equality is acceptance evidence, not an informational soft assertion."""
import copy,unittest
from test_model import own as ours
from model import initial,transition,GuardError,check_identity
class PanelGuard(unittest.TestCase):
    def test_stale_observation_refuses_initial_identity(self):
        s=initial('ble','abc123');o=ours();o['panelObservation']={'fresh':False}
        with self.assertRaisesRegex(GuardError,'panel'):check_identity(s,o,'ours')
    def test_panel_drift_is_not_success(self):
        s=initial('ble','abc123');s['state']='verify';before=ours();s['baseline']={'snapshot':before,'digest':'a'}
        after=copy.deepcopy(before);after['panel']['page']=3
        with self.assertRaisesRegex(GuardError,'panel'):transition(s,'verify',snapshot=after,digest='a')
if __name__=='__main__':unittest.main()

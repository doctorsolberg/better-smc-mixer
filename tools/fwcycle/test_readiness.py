"""Connection readiness precedes the one allowed UI Connect dispatch."""
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch
from bridge import BrowserAdapter
from test_adapter import FakeRPC
class Readiness(unittest.TestCase):
    def test_usb_waits_for_exact_enumerated_port(self):
        with tempfile.TemporaryDirectory() as t:
            rpc=FakeRPC();observed=[0]
            a=BrowserAdapter('http://127.0.0.1:1','http://127.0.0.1:8876',Path(t),rpc=rpc)
            a.poll_interval=0
            def ports():
                observed[0]+=1
                names=['SMC-Mixer -Private'] if observed[0]>=3 else []
                return {'inputs':names,'outputs':names}
            a.port_reader=ports
            a.action('stockConnect','usb')
            self.assertEqual(observed[0],3)
            self.assertEqual(sum(m=='action' for m,p in rpc.calls),1)
    def test_usb_timeout_is_structured_predispatch_not_ready(self):
        with tempfile.TemporaryDirectory() as t:
            rpc = FakeRPC()
            adapter = BrowserAdapter('unused', 'unused', Path(t), rpc=rpc)
            adapter.poll_interval = 0
            adapter.port_reader = lambda: {'inputs': [], 'outputs': []}
            with patch('bridge.time.monotonic', side_effect=[0, 31]):
                try:
                    result = adapter.action('stockConnect', 'usb')
                except TimeoutError as exc:
                    self.fail('prerequisite timeout must be structured, not terminal: ' + str(exc))
            self.assertIs(result.get('ready'), False)
            self.assertEqual(result['kind'], 'native_connection')
            self.assertEqual(result['expected_endpoint'], 'SMC-Mixer -Private')
            self.assertFalse(any(method == 'action' for method, _ in rpc.calls))

    def test_ble_not_ready_keeps_native_structured_prerequisite(self):
        with tempfile.TemporaryDirectory() as t:
            rpc = FakeRPC()
            adapter = BrowserAdapter('unused', 'unused', Path(t), rpc=rpc)
            adapter.owned = True
            prerequisite = {'ready': False, 'kind': 'disconnect_stock_midi',
                            'message': 'Disconnect only SMC-Mixer.'}
            with patch('macos.ensure_midi', return_value=prerequisite) as ensure:
                result = adapter.action('stockConnect', 'ble')
            self.assertEqual(result, prerequisite)
            ensure.assert_called_once_with('ble', 'stock')
            self.assertEqual(rpc.calls, [])

    def test_port_reader_timeout_is_predispatch_prerequisite(self):
        with tempfile.TemporaryDirectory() as t:
            rpc = FakeRPC()
            adapter = BrowserAdapter('unused', 'unused', Path(t), rpc=rpc)
            def ports():
                raise TimeoutError('enumeration deadline')
            adapter.port_reader = ports
            result = adapter.connection_readiness('oursConnect', 'usb')
            self.assertIs(result['ready'], False)
            self.assertEqual(result['kind'], 'midi_enumeration')
            self.assertEqual(rpc.calls, [])

    def test_native_enumerator_subprocess_timeout_is_prerequisite(self):
        import subprocess
        with tempfile.TemporaryDirectory() as t:
            rpc = FakeRPC()
            adapter = BrowserAdapter('unused', 'unused', Path(t), rpc=rpc)
            def ports():
                raise subprocess.TimeoutExpired('injected-enumerator', 15)
            adapter.port_reader = ports
            try:
                result = adapter.connection_readiness('oursConnect', 'usb')
            except subprocess.TimeoutExpired:
                self.fail('native enumeration timeout must remain a prerequisite result')
            self.assertIs(result['ready'], False)
            self.assertEqual(result['kind'], 'midi_enumeration')
            self.assertEqual(rpc.calls, [])

if __name__=='__main__':unittest.main()

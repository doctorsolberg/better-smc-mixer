"""Native UI identity guards, tested without macOS interaction."""
import unittest
from macos import choose_midi_row, forget_confirmation, native_checkpoint

class NativeTests(unittest.TestCase):
    def test_native_script_materializes_accessibility_lists_before_iteration(self):
        # System Events cannot coerce an inline `entire contents` query into a
        # repeat-item specifier (-1700). Captured on the real MIDI window.
        from macos import _BLE_SCRIPT
        self.assertNotIn('in (entire contents of', _BLE_SCRIPT)
        self.assertIn('set panelElements to entire contents of panel', _BLE_SCRIPT)
        self.assertIn('repeat with elementRef in panelElements', _BLE_SCRIPT)

    def test_duplicate_name_is_refused_even_when_one_action_is_unknown(self):
        rows = [{'name': 'SMC-Mixer', 'action': 'Disconnect'},
                {'name': 'SMC-Mixer', 'action': 'Unavailable'}]
        with self.assertRaises(ValueError):
            choose_midi_row(rows, 'SMC-Mixer')

    def test_disabled_exact_row_is_not_available(self):
        with self.assertRaises(ValueError):
            choose_midi_row([{'name': 'SMC-Mixer', 'action': 'Disconnect', 'enabled': False}], 'SMC-Mixer')

    def test_only_expected_available_row_selected(self):
        rows=[{'name':'SMC-Mixer','action':'Connect','id':1}, {'name':'Better-SMC-Mixer','action':'Connect','id':2}]
        self.assertEqual(choose_midi_row(rows,'Better-SMC-Mixer')['id'],2)
    def test_duplicate_or_offline_refused(self):
        row={'name':'Better-SMC-Mixer','action':'Connect'}
        with self.assertRaises(ValueError):choose_midi_row([row,row],'Better-SMC-Mixer')
        with self.assertRaises(ValueError):choose_midi_row([{'name':'Better-SMC-Mixer (Offline)','action':'Connect'}],'Better-SMC-Mixer')
    def test_usb_uses_config_port_not_ble_endpoint(self):
        ports={'inputs':['Better-SMC-Mixer Bluetooth'],'outputs':['Better-SMC-Mixer Bluetooth']}
        result=native_checkpoint('usb','ours',ports)
        self.assertEqual(result['expected_endpoint'],'SMC-Mixer Config')
        self.assertIsNone(native_checkpoint('usb','ours',{'inputs':['SMC-Mixer Config'],'outputs':['SMC-Mixer Config']}))
    def test_ble_uses_ble_port_not_usb_endpoint(self):
        result=native_checkpoint('ble','ours',{'inputs':['SMC-Mixer Config'],'outputs':['SMC-Mixer Config']})
        self.assertEqual(result['expected_endpoint'],'Better-SMC-Mixer Bluetooth')
    def test_stock_ble_does_not_request_system_midi_connect(self):
        self.assertIsNone(native_checkpoint('ble','stock',{'inputs':[],'outputs':[]}))
        result=native_checkpoint('ble','stock',{'inputs':['SMC-Mixer Bluetooth'],'outputs':[]})
        self.assertEqual(result['kind'],'disconnect_stock_midi')
    def test_forget_only_explicit_mixer_confirmation(self):
        self.assertTrue(forget_confirmation('Are you sure you want to forget “SMC-Mixer”?','SMC-Mixer'))
        self.assertFalse(forget_confirmation('Are you sure you want to forget “Keyboard”?','SMC-Mixer'))
        self.assertFalse(forget_confirmation('Are you sure you want to forget “SMC-Mixer”?','Keyboard'))
class EnsureMidiTests(unittest.TestCase):
    def test_pressed_connect_waits_for_endpoints_without_second_native_action(self):
        import macos
        from unittest.mock import patch
        calls=[];reads=[0]
        def executor(argv,**kwargs):
            calls.append(argv)
            if argv[0]=='/usr/bin/osascript':return type('R',(),{'stdout':'pressed\n'})()
            reads[0]+=1
            text='inputs:\noutputs:\n' if reads[0]<4 else 'inputs:\n  Better-SMC-Mixer Bluetooth\noutputs:\n  Better-SMC-Mixer Bluetooth\n'
            return type('R',(),{'stdout':text})()
        with patch('time.sleep'):
            self.assertEqual(macos.ensure_midi('ble','ours',executor=executor),{'ready':True})
        self.assertEqual(sum(a[0]=='/usr/bin/osascript' for a in calls),1)
        self.assertEqual(reads[0],4)

    def test_usb_enumerates_exact_endpoint_without_native_ui(self):
        import macos
        calls = []
        def executor(argv, **kwargs):
            calls.append((argv, kwargs))
            return type('Result', (), {'stdout': 'inputs:\n  SMC-Mixer Config\noutputs:\n  SMC-Mixer Config\n'})()
        self.assertEqual(macos.ensure_midi('usb', 'ours', executor=executor), {'ready': True})
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][0][-1], 'list')
        self.assertLessEqual(calls[0][1]['timeout'], 15)

    def test_ble_connects_only_ours_with_bounded_background_ax_then_verifies_ports(self):
        import macos
        calls = []
        def executor(argv, **kwargs):
            calls.append((argv, kwargs))
            if argv[0] == '/usr/bin/osascript':
                return type('Result', (), {'stdout': 'ready\n'})()
            if len(calls) == 1:
                return type('Result', (), {'stdout': 'inputs:\noutputs:\n'})()
            return type('Result', (), {'stdout': 'inputs:\n  Better-SMC-Mixer Bluetooth\noutputs:\n  Better-SMC-Mixer Bluetooth\n'})()
        self.assertEqual(macos.ensure_midi('ble', 'ours', executor=executor), {'ready': True})
        native = [c for c in calls if c[0][0] == '/usr/bin/osascript']
        self.assertEqual(len(native), 1)
        argv, kwargs = native[0]
        self.assertEqual(argv[-1], 'ours')
        self.assertIn('System Events', kwargs['input'])
        self.assertIn('Bluetooth Configuration', kwargs['input'])
        self.assertIn('perform action "AXPress"', kwargs['input'])
        self.assertNotIn('activate', kwargs['input'].lower())
        self.assertNotIn('keystroke', kwargs['input'].lower())
        self.assertLessEqual(kwargs['timeout'], 15)
        self.assertEqual(calls[-1][0][-1], 'list')

    def test_stock_disconnects_only_exact_stock_row_and_verifies_absent_endpoint(self):
        import macos
        calls = []
        def executor(argv, **kwargs):
            calls.append((argv, kwargs))
            if len(calls) == 1:
                return type('Result', (), {'stdout': 'inputs:\n  SMC-Mixer Bluetooth\noutputs:\n  SMC-Mixer Bluetooth\n'})()
            return type('Result', (), {'stdout': 'ready\n' if argv[0] == '/usr/bin/osascript' else 'inputs:\noutputs:\n'})()
        self.assertEqual(macos.ensure_midi('ble', 'stock', executor=executor), {'ready': True})
        self.assertEqual(calls[1][0], ['/usr/bin/osascript', '-', 'stock'])
        script = calls[1][1]['input']
        self.assertIn('set mixerName to "SMC-Mixer"', script)
        self.assertIn('set desiredLabel to "Connect"', script)
        self.assertIn('set desiredLabel to "Disconnect"', script)
        self.assertIn('my mixerRow(mixerName)', script)
        self.assertEqual(calls[-1][0][-1], 'list')

    def test_native_failures_become_manual_checkpoints_without_retry(self):
        import macos
        import subprocess
        for failure in (OSError('missing osascript'),
                        subprocess.CalledProcessError(1, ['osascript'], stderr='ambiguous row'),
                        subprocess.TimeoutExpired(['osascript'], 15)):
            with self.subTest(failure=type(failure).__name__):
                calls = []
                def executor(argv, **kwargs):
                    calls.append(argv)
                    if argv[0] != '/usr/bin/osascript':
                        return type('Result', (), {'stdout': 'inputs:\n  SMC-Mixer Bluetooth\noutputs:\n'})()
                    raise failure
                result = macos.ensure_midi('ble', 'stock', executor=executor)
                self.assertEqual(result['ready'], False)
                self.assertEqual(result['kind'], 'disconnect_stock_midi')
                self.assertIn('SMC-Mixer', result['message'])
                self.assertEqual(len(calls), 2)

    def test_enumeration_failure_is_not_ready_and_does_not_raise(self):
        import macos
        import subprocess
        def executor(argv, **kwargs):
            raise subprocess.TimeoutExpired(argv, 15)
        result = macos.ensure_midi('usb', 'ours', executor=executor)
        self.assertEqual(result['ready'], False)
        self.assertEqual(result['kind'], 'midi_enumeration')
        self.assertTrue(result['message'])

    def test_native_script_refuses_missing_or_ambiguous_table_and_offline_rows(self):
        import macos
        scripts = []
        def executor(argv, **kwargs):
            if argv[0] == '/usr/bin/osascript':
                scripts.append(kwargs['input'])
                return type('Result', (), {'stdout': 'unexpected_ui\n'})()
            if scripts:self.fail('must not enumerate after an unverified UI action')
            return type('Result', (), {'stdout': 'inputs:\noutputs:\n'})()
        result = macos.ensure_midi('ble', 'ours', executor=executor)
        self.assertFalse(result['ready'])
        self.assertIn('if (count tablesFound) is not 1 then error', scripts[0])
        self.assertIn('rows of (item 1 of tablesFound)', scripts[0])
        self.assertIn('Offline', scripts[0])
        self.assertIn('if (count matches) is not 1 then error', scripts[0])
        self.assertIn('if (count controls) is not 1 then error', scripts[0])
        self.assertEqual(scripts[0].count('perform action "AXPress"'), 1)

    def test_unrecognized_endpoint_listing_cannot_prove_stock_disconnected(self):
        import macos
        def executor(argv, **kwargs):
            return type('Result', (), {'stdout': 'ready\n' if argv[0] == '/usr/bin/osascript' else 'unrecognized output\n'})()
        result = macos.ensure_midi('ble', 'stock', executor=executor)
        self.assertFalse(result['ready'])
        self.assertEqual(result['kind'], 'midi_enumeration')

    def test_successful_native_status_requires_exact_endpoint_readback(self):
        import macos
        for phase, listing in (
                ('ours', 'inputs:\n  Better-SMC-Mixer Bluetooth\noutputs:\n'),
                ('ours', 'inputs:\n  SMC-Mixer Config\noutputs:\n  SMC-Mixer Config\n'),
                ('stock', 'inputs:\noutputs:\n  SMC-Mixer Bluetooth\n')):
            with self.subTest(phase=phase, listing=listing):
                calls = []
                def executor(argv, **kwargs):
                    calls.append(argv)
                    if len(calls) == 1:
                        initial = 'inputs:\noutputs:\n' if phase == 'ours' else listing
                        return type('Result', (), {'stdout': initial})()
                    return type('Result', (), {'stdout': 'ready\n' if argv[0] == '/usr/bin/osascript' else listing})()
                result = macos.ensure_midi('ble', phase, executor=executor)
                self.assertFalse(result['ready'])
                self.assertEqual(set(result), {'ready', 'kind', 'message'})
                self.assertEqual(len(calls), 3)

    def test_usb_stock_requires_both_private_ports_and_leaves_wait_to_parent(self):
        import macos
        calls = []
        def executor(argv, **kwargs):
            calls.append(argv)
            self.assertEqual(argv[-1], 'list')
            return type('Result', (), {'stdout': 'inputs:\n  SMC-Mixer -Private\noutputs:\n'})()
        self.assertEqual(macos.ensure_midi('usb', 'stock', executor=executor), {
            'ready': False, 'kind': 'native_connection',
            'message': 'Connect the USB cable to the mixer; do not pair Bluetooth for this USB run.'})
        self.assertEqual(len(calls), 1)

    def test_invalid_arguments_are_rejected_before_any_executor_call(self):
        import macos
        def executor(*args, **kwargs):
            self.fail('invalid request must not call OS executor')
        for transport, phase in (('BLE', 'ours'), ('wifi', 'ours'), ('ble', 'ours_connect'), ('usb', 'unknown')):
            with self.subTest(transport=transport, phase=phase), self.assertRaises(ValueError):
                macos.ensure_midi(transport, phase, executor=executor)

    def test_native_timeout_or_unknown_status_never_retries_click(self):
        import macos
        for status in ('transition_timeout', 'ready with warning', ''):
            calls = []
            def executor(argv, **kwargs):
                calls.append(argv)
                if argv[0] != '/usr/bin/osascript':
                    return type('Result', (), {'stdout': 'inputs:\noutputs:\n'})()
                return type('Result', (), {'stdout': status})()
            self.assertFalse(macos.ensure_midi('ble', 'ours', executor=executor)['ready'])
            self.assertEqual(len(calls), 2)

    def test_ready_ble_endpoints_skip_native_ui_entirely(self):
        import macos
        for phase, listing in (
                ('ours', 'inputs:\n  Better-SMC-Mixer Bluetooth\noutputs:\n  Better-SMC-Mixer Bluetooth\n'),
                ('stock', 'inputs:\n  SMC-Mixer Config\noutputs:\n  SMC-Mixer Config\n')):
            calls = []
            def executor(argv, **kwargs):
                calls.append(argv)
                self.assertEqual(argv[-1], 'list')
                return type('Result', (), {'stdout': listing})()
            self.assertEqual(macos.ensure_midi('ble', phase, executor=executor), {'ready': True})
            self.assertEqual(len(calls), 1)

if __name__=='__main__':unittest.main()

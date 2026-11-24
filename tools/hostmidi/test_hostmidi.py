"""Tests of tools/hostmidi without a MIDI device: the SysEx splitter, the byte-stream ports on a
socket pair, choosing a port by name, and that this OS's backend library loads with the functions it
calls (nothing is opened). Run: python3 -I tools/hostmidi/test_hostmidi.py
"""
import errno
import os
import platform
import queue
import socket
import sys
import threading
import unittest
from typing import Any
from unittest.mock import Mock, patch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostmidi  # noqa: E402


class SplitterTest(unittest.TestCase):
    def test_messages(self):
        s = hostmidi.SysexSplitter()
        self.assertEqual(s.feed(b"\x90\x40\x7F\xF0\x7D\x01"), [])
        self.assertEqual(s.feed(b"\xF8\x02\xF7\xB0\x07"), [b"\xF0\x7D\x01\x02\xF7"])  # real-time skipped
        self.assertEqual(s.feed(b"\xF0\x01\x90\x02\xF7"), [])  # cut short by a status
        self.assertEqual(s.feed(b"\xF7\x05\xF0\xF7"), [b"\xF0\xF7"])  # a lone F7, data outside


class StreamTest(unittest.TestCase):
    def test_both_ways(self):
        a, b = socket.socketpair()
        try:
            with hostmidi.open_port("fd:%d" % a.fileno()) as port:
                self.assertEqual(port.receive(0.05), [])
                b.sendall(b"\xF0\x01\x02")
                b.sendall(b"\x03\xF7\xF0\x04\xF7")
                got = []
                while len(got) < 2:
                    more = port.receive(1.0)
                    self.assertTrue(more)
                    got += more
                self.assertEqual(got, [b"\xF0\x01\x02\x03\xF7", b"\xF0\x04\xF7"])
                port.send(b"\xF0\x7D\x00\xF7")
                self.assertEqual(b.recv(16), b"\xF0\x7D\x00\xF7")
        finally:
            a.close()
            b.close()


class ChannelTest(unittest.TestCase):
    def test_parser(self):
        p = hostmidi.ChannelParser()
        msgs = p.feed(bytes([0x90, 0x3C, 0x7F, 0x3E, 0x7F,  # running status
                             0xF8,                          # clock, anywhere
                             0xB0, 0x07, 0xF8, 0x64,        # even inside a message
                             0xF0, 0x7D, 0x01, 0xF7,        # sysex: left out
                             0x40, 0x40,                    # no running status after it
                             0xC2, 0x05, 0x06,              # program change, running
                             0xE1, 0x00]))
        self.assertEqual([hostmidi.hex_of(x) for x in msgs],
                         ["90 3C 7F", "90 3E 7F", "B0 07 64", "C2 05", "C2 06"])
        self.assertEqual([hostmidi.hex_of(x) for x in p.feed(bytes([0x40]))], ["E1 00 40"])  # split across reads

    def test_capture(self):
        a, b = socket.socketpair()
        try:
            with hostmidi.open_port("fd:%d" % a.fileno()) as port:
                b.sendall(b"\x90\x01\x02")  # before capture(): not kept
                self.assertEqual(port.receive(0.1), [])
                port.capture()
                b.sendall(b"\x90\x3C\x7F\xF0\x7D\x05\xF7\x80\x3C")
                b.sendall(b"\x00")
                self.assertEqual(port.receive(1.0), [b"\xF0\x7D\x05\xF7"])  # SysEx still for receive()
                got = []
                for _ in range(100):
                    got += port.take()
                    if len(got) == 2:
                        break
                    port.receive(0.01)
                self.assertEqual([hostmidi.hex_of(m) for _, m in got], ["90 3C 7F", "80 3C 00"])
                self.assertLessEqual(got[0][0], got[1][0])
                b.sendall(b"\x90\x40\x01")
                port.receive(0.1)
                port.mark()
                self.assertEqual(port.take(), [])
        finally:
            a.close()
            b.close()


class PickTest(unittest.TestCase):
    def test_pick(self):
        names = ["Better-SMC-Mixer SMC-Mixer", "Better-SMC-Mixer SMC-Mixer Config", "IAC Bus 1"]
        self.assertEqual(hostmidi.pick(names, "SMC-Mixer Config", "x"), names[1])
        self.assertEqual(hostmidi.pick(names, "IAC Bus 1", "x"), names[2])
        with self.assertRaises(hostmidi.MidiError):
            hostmidi.pick(names, "SMC-Mixer", "x")  # two contain it, none is it
        with self.assertRaises(hostmidi.MidiError):
            hostmidi.pick(names, "nothing", "x")


class CoreMidiNamesTest(unittest.TestCase):
    """CoreMIDI may shorten the USB endpoints' names to an empty string and 'Config'."""

    def names(self, properties, broken=()):
        strings, released, keys = {}, [], []

        class CF:
            def CFStringCreateWithCString(self, allocator, value, encoding):
                keys.append(value)
                return value

            def CFStringGetCString(self, value, buf, size, encoding):
                value = getattr(value, 'value', value)
                if strings[value] in broken:
                    return False
                buf.value = strings[value]
                return True

            def CFRelease(self, value):
                released.append(getattr(value, 'value', value))

        class CM:
            def MIDIGetNumberOfSources(self):
                return len(properties)

            MIDIGetNumberOfDestinations = MIDIGetNumberOfSources

            def MIDIGetSource(self, index):
                return index

            MIDIGetDestination = MIDIGetSource

            def MIDIObjectGetStringProperty(self, ref, key, result):
                value = properties[ref].get(key)
                if value is None:
                    return -1
                token = len(strings) + 1
                strings[token] = value
                result._obj.value = token
                return 0

        result = hostmidi.coremidi_names(CM(), CF())
        self.assertEqual(sorted(x for x in released if isinstance(x, int)), sorted(strings))
        self.assertEqual(sorted(x for x in released if isinstance(x, bytes)), sorted(keys))
        return result

    def test_visible_names_when_short_names_are_empty_or_generic(self):
        result = self.names([{b'name': b'', b'displayName': b'SMC-Mixer'},
                             {b'name': b'Config', b'displayName': b'SMC-Mixer Config'}])
        expected = [(0, 'SMC-Mixer'), (1, 'SMC-Mixer Config')]
        self.assertEqual(result, {'in': expected, 'out': expected})

    def test_fallback_to_name(self):
        result = self.names([{b'name': b'Legacy'}, {b'name': b'Config', b'displayName': b''}])
        self.assertEqual(result['in'], [(0, 'Legacy'), (1, 'Config')])

    def test_failed_conversion_and_missing_names(self):
        result = self.names([{b'name': b'Fallback', b'displayName': b'broken'}, {}], broken=(b'broken',))
        self.assertEqual(result['out'], [(0, 'Fallback')])


class AlsaSendTest(unittest.TestCase):
    """Call the real send loop with a fake ALSA API, never opening a native port."""

    def port(self) -> Any:
        port = hostmidi.AlsaPort.__new__(hostmidi.AlsaPort)
        hostmidi.Port.__init__(port, "Fake ALSA")
        port.outp = hostmidi.ctypes.c_void_p(1)
        port.a = Mock()
        port.a.snd_rawmidi_drain.return_value = 0
        return port

    def test_permanent_write_error_is_not_retried(self):
        port = self.port()
        # A second call fails the test instead of hanging on the old infinite loop.
        port.a.snd_rawmidi_write.side_effect = [-errno.ENODEV, AssertionError("permanent error retried")]
        with self.assertRaisesRegex(hostmidi.MidiError, "ALSA.*write.*ENODEV"):
            port.send(b"\xF0\x01\xF7")
        self.assertEqual(port.a.snd_rawmidi_write.call_count, 1)
        port.a.snd_rawmidi_drain.assert_not_called()

    def test_transient_write_wait_is_bounded(self):
        for result in (-errno.EAGAIN, -errno.EINTR, 0):
            with self.subTest(result=result):
                port = self.port()
                port.a.snd_rawmidi_write.return_value = result
                now = [0.0]

                def sleep(seconds):
                    now[0] += seconds
                    self.assertLessEqual(now[0], 2.001, "send exceeded its operation deadline")

                with patch.object(hostmidi.time, "monotonic", side_effect=lambda: now[0]), \
                     patch.object(hostmidi.time, "sleep", side_effect=sleep):
                    with self.assertRaisesRegex(hostmidi.MidiError, "ALSA.*write.*timed out"):
                        port.send(b"\xF0\x01\xF7")
                self.assertGreaterEqual(now[0], 2.0)
                port.a.snd_rawmidi_drain.assert_not_called()

    def test_drain_errors_are_reported_and_wait_is_bounded(self):
        for result, message in ((-errno.ENODEV, "ENODEV"), (-errno.EAGAIN, "timed out"),
                                (-errno.EINTR, "timed out")):
            with self.subTest(result=result):
                port = self.port()
                port.a.snd_rawmidi_write.return_value = 3
                port.a.snd_rawmidi_drain.return_value = result
                now = [0.0]

                def sleep(seconds):
                    now[0] += seconds
                    self.assertLessEqual(now[0], 2.001, "drain exceeded its operation deadline")

                with patch.object(hostmidi.time, "monotonic", side_effect=lambda: now[0]), \
                     patch.object(hostmidi.time, "sleep", side_effect=sleep):
                    with self.assertRaisesRegex(hostmidi.MidiError, "ALSA.*drain.*" + message):
                        port.send(b"\xF0\x01\xF7")
                if result == -errno.ENODEV:
                    self.assertEqual(port.a.snd_rawmidi_drain.call_count, 1)

    def test_partial_writes_and_transient_errors_preserve_bytes(self):
        port = self.port()
        port.a.snd_rawmidi_write.side_effect = [-errno.EAGAIN, 2, -errno.EINTR, 0, 3]
        port.a.snd_rawmidi_drain.side_effect = [-errno.EAGAIN, -errno.EINTR, 0]
        message = b"\xF0\x7D\x01\x02\xF7"
        with patch.object(hostmidi.time, "sleep"):
            port.send(message)
        self.assertEqual([c.args[1:] for c in port.a.snd_rawmidi_write.call_args_list],
                         [(message, 5), (message, 5)] + [(message[2:], 3)] * 3)
        self.assertEqual(port.a.snd_rawmidi_drain.call_count, 3)


class WinMmInputTest(unittest.TestCase):
    def test_short_messages_and_sysex_use_worker_in_callback_order(self):
        port = hostmidi.WinMmPort.__new__(hostmidi.WinMmPort)
        hostmidi.Port.__init__(port, "Fake WinMM")
        port.done = queue.SimpleQueue()
        port.stop = threading.Event()
        port.hin = hostmidi.ctypes.c_void_p(1)
        port.w = Mock()
        sysex = b"\xF0\x7D\x01\xF7"
        data = hostmidi.ctypes.create_string_buffer(sysex)
        header = hostmidi.MIDIHDR(lpData=hostmidi.ctypes.addressof(data), dwBytesRecorded=len(sysex))
        port.headers = [(header, data)]
        port.capture()
        processed, requeued = [], []
        all_processed = threading.Event()
        real_bytes = port._bytes

        def read_bytes(data):
            processed.append(bytes(data))
            real_bytes(data)
            if len(processed) == len(expected):
                all_processed.set()

        def requeue(handle, pointer, size):
            requeued.append(pointer._obj)
            self.assertEqual(threading.current_thread().name, "test-winmm-worker")

        port._bytes = read_bytes
        port.w.midiInAddBuffer.side_effect = requeue
        expected = [b"\x90\x3C\x7F", b"\xB1\x07\x64", b"\xC2\x05", sysex,
                    b"\xD3\x40", b"\x80\x3C\x00"]
        for message in expected:
            if message == sysex:
                port._callback(None, hostmidi.MIM_LONGDATA, 0, hostmidi.ctypes.addressof(header), 0)
            else:
                # High padding is deliberately nonzero: Program/Channel Pressure are two bytes.
                packed = int.from_bytes(message.ljust(4, b"\x66"), "little")
                port._callback(None, 0x3C3, 0, packed, 0)  # MIM_DATA
        self.assertEqual(processed, [], "the callback must only enqueue")
        port.w.midiInAddBuffer.assert_not_called()
        self.assertEqual(port.take(), [])
        worker = threading.Thread(target=port._run, name="test-winmm-worker")
        worker.start()
        try:
            self.assertEqual(port.receive(1.0), [sysex])
            self.assertTrue(all_processed.wait(1.0), "the worker lost queued MIDI input")
        finally:
            port.stop.set()
            worker.join(1.0)
        self.assertFalse(worker.is_alive())
        self.assertEqual(processed, expected)
        captured = port.take()
        self.assertEqual([m for _, m in captured], [m for m in expected if m != sysex])
        self.assertEqual([t for t, _ in captured], sorted(t for t, _ in captured))
        self.assertEqual(requeued, [header])


class BackendTest(unittest.TestCase):
    """The OS's library and the functions the backend calls; nothing is opened or listed."""

    def test_library(self):
        system = platform.system()
        if system == "Darwin":
            cm, _ = hostmidi.coremidi_libs()
            for f in ("MIDIClientCreate", "MIDIInputPortCreate", "MIDIOutputPortCreate", "MIDISend",
                      "MIDIGetDestination", "MIDIPortConnectSource"):
                self.assertTrue(hasattr(cm, f), f)
        elif system == "Linux":
            try:
                a = hostmidi.alsa_lib()
            except hostmidi.MidiError as e:
                self.skipTest(str(e))
            for f in ("snd_rawmidi_open", "snd_ctl_rawmidi_next_device", "snd_rawmidi_info_get_subdevice_name"):
                self.assertTrue(hasattr(a, f), f)
        elif system == "Windows":
            w = hostmidi.winmm_lib()
            for f in ("midiInOpen", "midiInAddBuffer", "midiOutLongMsg", "midiOutGetDevCapsW"):
                self.assertTrue(hasattr(w, f), f)
        else:
            self.skipTest("no backend for %s" % system)
        self.assertEqual(hostmidi.MIDIHDR.dwReserved.offset + 8 * hostmidi.ctypes.sizeof(hostmidi.ctypes.c_size_t),
                         hostmidi.ctypes.sizeof(hostmidi.MIDIHDR))


if __name__ == "__main__":
    unittest.main()

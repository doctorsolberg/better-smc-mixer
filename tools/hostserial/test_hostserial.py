#!/usr/bin/env python3
"""Tests of hostserial.py: opening and moving bytes on a pseudo-terminal (POSIX), and finding the
mixer's port in canned Linux sysfs trees, macOS ioreg output and Windows registry contents."""
import os
import plistlib
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostserial as hs  # noqa: E402


@unittest.skipUnless(hasattr(os, "openpty"), "no pseudo-terminals on this OS")
class OnAPty(unittest.TestCase):
    def test_both_ways(self):
        master, slave = os.openpty()
        port = hs.open_port(os.ttyname(slave))
        try:
            self.assertEqual(port.read(0.05), b"")  # nothing yet: returns in time, empty
            port.write(bytes(range(256)))  # raw: every byte as it is, no echo, no line rules
            got = b""
            while len(got) < 256:
                got += os.read(master, 4096)
            self.assertEqual(got, bytes(range(256)))
            os.write(master, b"\x00\x0a\x0d\x03\x11\x13\xff")
            data = b""
            for _ in range(20):
                data += port.read(0.1)
                if len(data) >= 7:
                    break
            self.assertEqual(data, b"\x00\x0a\x0d\x03\x11\x13\xff")
        finally:
            port.close()
            os.close(slave)
            os.close(master)


class Linux(unittest.TestCase):
    def tree(self, root, name, vid, pid, depth=2):
        usb = os.path.join(root, "devices", "usb1", "1-1")
        iface = usb
        for i in range(depth):
            iface = os.path.join(iface, "1-1:1.%d" % i)
        os.makedirs(iface, exist_ok=True)
        with open(os.path.join(usb, "idVendor"), "w") as f:
            f.write("%04x\n" % vid)
        with open(os.path.join(usb, "idProduct"), "w") as f:
            f.write("%04x\n" % pid)
        cls = os.path.join(root, "class", "tty", name)
        os.makedirs(cls)
        os.symlink(iface, os.path.join(cls, "device"))

    def test_sysfs(self):
        with tempfile.TemporaryDirectory() as root:
            self.tree(root, "ttyACM0", 0x1209, 0x0001)
            os.makedirs(os.path.join(root, "class", "tty", "tty0"))  # a console: no device
            found = hs.linux_ports(root, "/dev")
            self.assertEqual(found, [("/dev/ttyACM0", 0x1209, 0x0001)])


class MacOS(unittest.TestCase):
    def test_ioreg(self):
        xml = plistlib.dumps([
            {"idVendor": 0x1209, "idProduct": 0x0001, "IORegistryEntryChildren": [
                {"IORegistryEntryChildren": [{"IOCalloutDevice": "/dev/cu.usbmodem1101", "IODialinDevice": "/dev/tty.usbmodem1101"}]}]},
            {"idVendor": 0x05AC, "idProduct": 0x1234, "IORegistryEntryChildren": []},
            {"idVendor": 0x2E8A, "idProduct": 0x000A, "IORegistryEntryChildren": [{"IOCalloutDevice": "/dev/cu.usbmodem2201"}]},
        ])
        self.assertEqual(hs.macos_ports(xml), [("/dev/cu.usbmodem1101", 0x1209, 0x0001),
                                               ("/dev/cu.usbmodem2201", 0x2E8A, 0x000A)])
        self.assertEqual(hs.macos_ports(b"not a plist"), [])


class Windows(unittest.TestCase):
    def test_registry(self):
        base = r"SYSTEM\CurrentControlSet\Enum\USB"
        keys = {
            base: (["VID_1209&PID_0001&MI_00", "VID_046D&PID_C52B", "ROOT_HUB30"], {}),
            base + r"\VID_1209&PID_0001&MI_00": (["6&1a2b3c&0&0000"], {}),
            base + r"\VID_1209&PID_0001&MI_00\6&1a2b3c&0&0000\Device Parameters": ([], {"PortName": "COM7"}),
            base + r"\VID_046D&PID_C52B": (["5&abc"], {}),
            base + r"\VID_046D&PID_C52B\5&abc\Device Parameters": ([], {"SymbolicName": "x"}),
        }
        self.assertEqual(hs.windows_ports(keys.get), [("COM7", 0x1209, 0x0001)])


@unittest.skipUnless(hasattr(os, "openpty"), "no pseudo-terminals on this OS")
class PortAuto(unittest.TestCase):
    def test_smclog_port_auto(self):
        # every tool's --port goes through smclog.open_serial: "auto" finds the port by its IDs
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "smclog"))
        import smclog
        master, slave = os.openpty()
        real = hs.ports
        try:
            hs.ports = lambda: [(os.ttyname(slave), hs.VID, hs.PID)]
            fd = smclog.open_serial("auto")
            os.write(fd, b"\x01\x02")
            self.assertEqual(os.read(master, 2), b"\x01\x02")
            os.close(fd)
        finally:
            hs.ports = real
            os.close(slave)
            os.close(master)


class Find(unittest.TestCase):
    def test_find_one(self):
        real = hs.ports
        try:
            hs.ports = lambda: [("/dev/a", 0x1209, 0x0001), ("/dev/b", 0x2E8A, 0x000A)]
            self.assertEqual(hs.find_one(), "/dev/a")
            hs.ports = lambda: []
            with self.assertRaisesRegex(OSError, "no serial port"):
                hs.find_one()
            hs.ports = lambda: [("/dev/a", 0x1209, 1), ("/dev/c", 0x1209, 1)]
            with self.assertRaisesRegex(OSError, "more than one.*name the port"):
                hs.find_one()
        finally:
            hs.ports = real


if __name__ == "__main__":
    unittest.main()

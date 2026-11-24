#!/usr/bin/env python3
"""Serial ports on the host, for the command link over the mixer's CDC port (docs/link.md;
docs/integration.md, "Any OS"): one interface on every OS, standard library only.

  path = hostserial.find_one()            # the mixer's CDC port, by its USB IDs (1209:0001)
  port = hostserial.open_port(path)       # raw, 8 data bits, no flow control
  port.write(b"...")
  data = port.read(0.5)                   # what arrived within 0.5 s (b"" if nothing)
  port.close()

Finding a port by its USB vendor and product IDs:

- Linux: sysfs, /sys/class/tty/<name>/device up to the USB device's idVendor and idProduct;
  the port is /dev/<name> (ttyACM0).
- macOS: `ioreg -a -r -c IOUSBHostDevice -l` (part of macOS), the device's IOCalloutDevice
  (/dev/cu.usbmodem...).
- Windows: the registry (winreg), HKLM\\SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_1209&PID_0001\\<serial>\\
  Device Parameters\\PortName (COM3); a composite device's interfaces end in &MI_xx.

Opening: POSIX through termios (raw, as tty.setraw, CLOCAL and CREAD; a pseudo-terminal too, which
the tests use); Windows through CreateFileW, SetCommState and SetCommTimeouts by ctypes. A port is
only read and written: no DTR or baud games (the CDC ignores the baud rate).

  python3 -I tools/hostserial/hostserial.py list [--all]   the mixer's ports (--all: every USB
                                                            serial port, with its IDs)

✅ POSIX opening on a pseudo-terminal (the tests); ⚠️ the Linux, macOS and Windows discovery are
tested on canned sysfs trees, ioreg output and registry contents, not on those systems with the
mixer; ❓ the Win32 opening has not run at all (no Windows host here).
"""
import os
import plistlib
import subprocess
import sys

VID, PID = 0x1209, 0x0001  # the pid.codes test ID of development builds (docs/link.md; SPEC Q-14)


# --- finding ports ---------------------------------------------------------------------------------

def _read(path):
    try:
        with open(path, encoding="ascii", errors="replace") as f:
            return f.read().strip()
    except OSError:
        return None


def linux_ports(sys_root="/sys", dev_root="/dev"):
    """[(port path, vid, pid)] of the USB serial ports sysfs lists."""
    out = []
    base = os.path.join(sys_root, "class", "tty")
    try:
        names = sorted(os.listdir(base))
    except OSError:
        return out
    for name in names:
        dev = os.path.join(base, name, "device")
        if not os.path.exists(dev):
            continue
        d = os.path.realpath(dev)
        for _ in range(6):  # the interface, then the USB device holding idVendor
            v, p = _read(os.path.join(d, "idVendor")), _read(os.path.join(d, "idProduct"))
            if v and p:
                try:
                    out.append((os.path.join(dev_root, name), int(v, 16), int(p, 16)))
                except ValueError:
                    pass
                break
            d = os.path.dirname(d)
    return out


def _walk(node):
    yield node
    for child in node.get("IORegistryEntryChildren", []) or []:
        yield from _walk(child)


def macos_ports(ioreg_xml=None):
    """[(port path, vid, pid)] from ioreg's plist of the USB devices."""
    if ioreg_xml is None:
        try:
            ioreg_xml = subprocess.run(["ioreg", "-a", "-r", "-c", "IOUSBHostDevice", "-l"],
                                       capture_output=True, timeout=10).stdout
        except (OSError, subprocess.SubprocessError):
            return []
    if not ioreg_xml:
        return []
    try:
        roots = plistlib.loads(ioreg_xml)
    except Exception:  # noqa: BLE001 - ioreg's output is not ours
        return []
    out = []
    for dev in roots if isinstance(roots, list) else [roots]:
        vid, pid = dev.get("idVendor"), dev.get("idProduct")
        if vid is None or pid is None:
            continue
        for n in _walk(dev):
            path = n.get("IOCalloutDevice")
            if path:
                out.append((path, vid, pid))
    return out


def windows_ports(registry=None):
    """[(port name, vid, pid)] from the registry's USB enumeration. registry: a function
    (key path) -> ([subkey names], {value name: value}) or None, for the tests; else winreg."""
    if registry is None:
        try:
            import winreg
        except ImportError:
            return []

        def registry(path):
            try:
                with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, path) as k:
                    n_sub, n_val, _ = winreg.QueryInfoKey(k)
                    subs = [winreg.EnumKey(k, i) for i in range(n_sub)]
                    vals = {}
                    for i in range(n_val):
                        name, value, _ = winreg.EnumValue(k, i)
                        vals[name] = value
                    return subs, vals
            except OSError:
                return None
    base = r"SYSTEM\CurrentControlSet\Enum\USB"
    top = registry(base)
    if not top:
        return []
    out = []
    for key in top[0]:
        parts = dict(p.split("_", 1) for p in key.upper().split("&") if "_" in p)
        try:
            vid, pid = int(parts["VID"], 16), int(parts["PID"], 16)
        except (KeyError, ValueError):
            continue
        inst = registry(base + "\\" + key)
        for serial in (inst[0] if inst else []):
            params = registry(base + "\\" + key + "\\" + serial + "\\Device Parameters")
            name = params[1].get("PortName") if params else None
            if isinstance(name, str) and name.upper().startswith("COM"):
                out.append((name, vid, pid))
    return out


def ports():
    """Every USB serial port this OS lists: [(path, vid, pid)]."""
    if sys.platform.startswith("linux"):
        return linux_ports()
    if sys.platform == "darwin":
        return macos_ports()
    if sys.platform == "win32":
        return windows_ports()
    return []


def find(vid=VID, pid=PID):
    """The ports of the device with these USB IDs."""
    return [p for p, v, d in ports() if (v, d) == (vid, pid)]


def find_one(vid=VID, pid=PID):
    found = find(vid, pid)
    if len(found) != 1:
        raise OSError("%s serial port with USB IDs %04x:%04x%s" % (
            "no" if not found else "more than one", vid, pid,
            "" if not found else " (%s): name the port" % ", ".join(found)))
    return found[0]


# --- opening ports ---------------------------------------------------------------------------------

class PosixPort:
    def __init__(self, path):
        import termios
        import tty
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        if os.isatty(self.fd):
            tty.setraw(self.fd, termios.TCSANOW)
            attrs = termios.tcgetattr(self.fd)
            attrs[2] |= termios.CLOCAL | termios.CREAD
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)

    def fileno(self):
        return self.fd

    def write(self, data):
        import select
        data = memoryview(bytes(data))
        while data:
            select.select([], [self.fd], [], 1.0)
            try:
                n = os.write(self.fd, data)
            except BlockingIOError:
                continue
            data = data[n:]

    def read(self, timeout):
        import select
        ready, _, _ = select.select([self.fd], [], [], timeout)
        if not ready:
            return b""
        try:
            data = os.read(self.fd, 4096)
        except BlockingIOError:
            return b""
        if data == b"":
            raise OSError("the port was closed")
        return data

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None


class WindowsPort:
    """❓ not run: no Windows host here."""

    def __init__(self, name):
        import ctypes
        from ctypes import wintypes
        self.k32 = k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.CreateFileW.restype = wintypes.HANDLE
        path = name if name.startswith("\\\\.\\") else "\\\\.\\" + name
        GENERIC_RW, OPEN_EXISTING = 0x80000000 | 0x40000000, 3
        h = k32.CreateFileW(path, GENERIC_RW, 0, None, OPEN_EXISTING, 0, None)
        if h in (None, wintypes.HANDLE(-1).value):
            raise OSError("cannot open %s: Windows error %d" % (name, ctypes.get_last_error()))
        self.h = h

        class DCB(ctypes.Structure):
            _fields_ = [("DCBlength", wintypes.DWORD), ("BaudRate", wintypes.DWORD), ("flags", wintypes.DWORD),
                        ("wReserved", wintypes.WORD), ("XonLim", wintypes.WORD), ("XoffLim", wintypes.WORD),
                        ("ByteSize", ctypes.c_ubyte), ("Parity", ctypes.c_ubyte), ("StopBits", ctypes.c_ubyte),
                        ("XonChar", ctypes.c_char), ("XoffChar", ctypes.c_char), ("ErrorChar", ctypes.c_char),
                        ("EofChar", ctypes.c_char), ("EvtChar", ctypes.c_char), ("wReserved1", wintypes.WORD)]

        class TIMEOUTS(ctypes.Structure):
            _fields_ = [(n, wintypes.DWORD) for n in ("ReadIntervalTimeout", "ReadTotalTimeoutMultiplier",
                                                      "ReadTotalTimeoutConstant", "WriteTotalTimeoutMultiplier",
                                                      "WriteTotalTimeoutConstant")]
        dcb = DCB()
        dcb.DCBlength = ctypes.sizeof(DCB)
        if not k32.GetCommState(h, ctypes.byref(dcb)):
            self.close()
            raise OSError("%s is not a serial port" % name)
        dcb.BaudRate, dcb.ByteSize, dcb.Parity, dcb.StopBits = 115200, 8, 0, 0
        dcb.flags = 0x1 | (0x1 << 4)  # fBinary; fDtrControl = DTR_CONTROL_ENABLE (bits 4-5)
        k32.SetCommState(h, ctypes.byref(dcb))
        self.TIMEOUTS, self.ctypes, self.wintypes = TIMEOUTS, ctypes, wintypes
        self._timeouts(0)

    def _timeouts(self, ms):
        t = self.TIMEOUTS()
        if ms == 0:  # return at once with what is there
            t.ReadIntervalTimeout = 0xFFFFFFFF
        else:  # wait up to ms for the first byte, then return what arrived
            t.ReadIntervalTimeout, t.ReadTotalTimeoutMultiplier, t.ReadTotalTimeoutConstant = 0xFFFFFFFF, 0xFFFFFFFF, ms
        t.WriteTotalTimeoutConstant = 1000
        self.k32.SetCommTimeouts(self.h, self.ctypes.byref(t))

    def write(self, data):
        data = bytes(data)
        n = self.wintypes.DWORD()
        while data:
            if not self.k32.WriteFile(self.h, data, len(data), self.ctypes.byref(n), None):
                raise OSError("write: Windows error %d" % self.ctypes.get_last_error())
            data = data[n.value:]

    def read(self, timeout):
        self._timeouts(max(1, int(timeout * 1000)) if timeout > 0 else 0)
        buf = self.ctypes.create_string_buffer(4096)
        n = self.wintypes.DWORD()
        if not self.k32.ReadFile(self.h, buf, 4096, self.ctypes.byref(n), None):
            raise OSError("read: Windows error %d" % self.ctypes.get_last_error())
        return buf.raw[:n.value]

    def close(self):
        if self.h is not None:
            self.k32.CloseHandle(self.h)
            self.h = None


def open_port(path):
    """An open port: "auto" finds the mixer's by its USB IDs."""
    if path == "auto":
        path = find_one()
    return WindowsPort(path) if sys.platform == "win32" else PosixPort(path)


def main(argv):
    if not argv or argv[0] != "list":
        print(__doc__.split("\n\n")[0])
        print("usage: hostserial.py list [--all]")
        return 2
    every = "--all" in argv
    found = [(p, v, d) for p, v, d in ports() if every or (v, d) == (VID, PID)]
    for p, v, d in found:
        print("%s  %04x:%04x" % (p, v, d))
    if not found:
        print("no %s" % ("USB serial port" if every else "port with USB IDs %04x:%04x (is the mixer connected?)" % (VID, PID)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

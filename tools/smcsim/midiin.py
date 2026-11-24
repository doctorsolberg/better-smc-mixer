"""MIDI the host receives from the device, for smcsim device (SPEC LOG-35; docs/core.md, "Scenarios
on the device"): the scenario runner compares it with the MIDI the device logged.

Sources (--midi SOURCE):

- raw:PATH   a raw MIDI byte stream: an ALSA rawmidi device on Linux (/dev/snd/midiC1D0), or the
             pseudo-terminal build/host/devhost writes its MIDI to (--midi there). Read in a thread.
- coremidi:NAME   on macOS, the CoreMIDI source named NAME ("SMC-Mixer", the performance port; the
             configuration port is "SMC-Mixer Config"), through ctypes. ⚠️ Not run yet: it needs the
             device; the tests only check that the framework has the functions it calls.

Each message is kept with the host's monotonic time in ns when it was read. Channel messages only:
real-time bytes and system exclusive are left out (the comparison is about the core's MIDI).
Standard library only.
"""
import ctypes
import ctypes.util
import os
import platform
import select
import threading
import time


class MidiError(Exception):
    pass


DATA_BYTES = {0x8: 2, 0x9: 2, 0xA: 2, 0xB: 2, 0xC: 1, 0xD: 1, 0xE: 2}


class Parser:
    """A MIDI byte stream to channel messages (running status kept; system common and exclusive
    messages clear it and are skipped, real-time bytes are skipped wherever they come)."""

    def __init__(self):
        self.status = None
        self.data = []
        self.in_sysex = False

    def feed(self, data):
        out = []
        for b in data:
            if b >= 0xF8:  # real-time: anywhere, no effect on the rest
                continue
            if b >= 0x80:
                self.data = []
                if b == 0xF0:
                    self.in_sysex, self.status = True, None
                elif b >= 0xF0:  # F7 ends a sysex; other system common: no running status
                    self.in_sysex, self.status = False, None
                else:
                    self.in_sysex, self.status = False, b
                continue
            if self.in_sysex or self.status is None:
                continue
            self.data.append(b)
            if len(self.data) == DATA_BYTES[self.status >> 4]:
                out.append(bytes([self.status] + self.data))
                self.data = []
        return out


def hex_of(msg):
    return " ".join("%02X" % b for b in msg)


class Source:
    """Collects (t_ns, message bytes) from a start mark on."""

    def __init__(self):
        self.lock = threading.Lock()
        self.messages = []
        self.parser = Parser()

    def _bytes(self, data, t_ns=None):
        t = time.monotonic_ns() if t_ns is None else t_ns
        msgs = self.parser.feed(data)
        if msgs:
            with self.lock:
                self.messages += [(t, m) for m in msgs]

    def mark(self):
        """From now on: drops what came before."""
        with self.lock:
            self.messages = []

    def take(self):
        with self.lock:
            out, self.messages = self.messages, []
        return out

    def close(self):
        pass


class RawSource(Source):
    """A file, pseudo-terminal or rawmidi device read in a thread."""

    def __init__(self, path):
        super().__init__()
        try:
            self.fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_NOCTTY", 0))
        except OSError as e:
            raise MidiError("cannot open the MIDI source %s: %s" % (path, e))
        if os.isatty(self.fd):
            import tty
            tty.setraw(self.fd)  # bytes as they are
        self.path = path
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, name="midi-in", daemon=True)
        self.thread.start()

    def _run(self):
        while not self.stop.is_set():
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if not r:
                continue
            try:
                data = os.read(self.fd, 4096)
            except BlockingIOError:
                continue
            except OSError:
                return
            if not data:
                time.sleep(0.01)  # a pipe's writer gone, or nothing: keep polling until closed
                continue
            self._bytes(data)

    def close(self):
        self.stop.set()
        self.thread.join(1)
        os.close(self.fd)


# --- CoreMIDI (macOS) --------------------------------------------------------------------------

K_UTF8 = 0x08000100  # kCFStringEncodingUTF8
READ_PROC = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)


def coremidi_libs():
    """(CoreMIDI, CoreFoundation) with the signatures used here; MidiError if absent."""
    if platform.system() != "Darwin":
        raise MidiError("coremidi: only on macOS")
    paths = [ctypes.util.find_library(n) for n in ("CoreMIDI", "CoreFoundation")]
    if not all(paths):
        raise MidiError("coremidi: CoreMIDI or CoreFoundation not found")
    cm, cf = ctypes.cdll.LoadLibrary(paths[0]), ctypes.cdll.LoadLibrary(paths[1])
    vp, u32, i32 = ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int32
    cf.CFStringCreateWithCString.restype = vp
    cf.CFStringCreateWithCString.argtypes = [vp, ctypes.c_char_p, u32]
    cf.CFStringGetCString.restype = ctypes.c_bool
    cf.CFStringGetCString.argtypes = [vp, ctypes.c_char_p, ctypes.c_long, u32]
    cf.CFRelease.argtypes = [vp]
    cm.MIDIClientCreate.restype = i32
    cm.MIDIClientCreate.argtypes = [vp, vp, vp, ctypes.POINTER(u32)]
    cm.MIDIInputPortCreate.restype = i32
    cm.MIDIInputPortCreate.argtypes = [u32, vp, READ_PROC, vp, ctypes.POINTER(u32)]
    cm.MIDIGetNumberOfSources.restype = ctypes.c_ulong
    cm.MIDIGetSource.restype = u32
    cm.MIDIGetSource.argtypes = [ctypes.c_ulong]
    cm.MIDIObjectGetStringProperty.restype = i32
    cm.MIDIObjectGetStringProperty.argtypes = [u32, vp, ctypes.POINTER(vp)]
    cm.MIDIPortConnectSource.restype = i32
    cm.MIDIPortConnectSource.argtypes = [u32, u32, vp]
    cm.MIDIPortDisconnectSource.argtypes = [u32, u32]
    cm.MIDIPortDispose.argtypes = [u32]
    cm.MIDIClientDispose.argtypes = [u32]
    return cm, cf


def packets(buf_addr):
    """(timestamp, bytes) of each packet of a MIDIPacketList at buf_addr. MIDIPacket: UInt64
    timeStamp, UInt16 length, data; packed to 4 bytes on Apple silicon (MIDIPacketNext)."""
    count = ctypes.c_uint32.from_address(buf_addr).value
    off = buf_addr + 4
    align = 4 if platform.machine() == "arm64" else 1
    out = []
    for _ in range(count):
        ts = ctypes.c_uint64.from_address(off).value
        n = ctypes.c_uint16.from_address(off + 8).value
        out.append((ts, ctypes.string_at(off + 10, n)))
        off = (off + 10 + n + align - 1) // align * align
    return out


class CoreMidiSource(Source):
    def __init__(self, name):
        super().__init__()
        self.cm, self.cf = coremidi_libs()
        cm, cf = self.cm, self.cf

        def cfstr(s):
            return cf.CFStringCreateWithCString(None, s.encode(), K_UTF8)

        self.client, self.port = ctypes.c_uint32(), ctypes.c_uint32()
        cname, pname = cfstr("smcsim"), cfstr("smcsim in")
        try:
            if cm.MIDIClientCreate(cname, None, None, ctypes.byref(self.client)):
                raise MidiError("coremidi: MIDIClientCreate failed")
            self.proc = READ_PROC(self._read)  # kept: CoreMIDI calls it from its own thread
            if cm.MIDIInputPortCreate(self.client.value, pname, self.proc, None, ctypes.byref(self.port)):
                raise MidiError("coremidi: MIDIInputPortCreate failed")
        finally:
            cf.CFRelease(cname)
            cf.CFRelease(pname)
        names = self.sources()
        hits = [ref for ref, n in names if n == name]
        if len(hits) != 1:
            self.close()
            raise MidiError("coremidi: %d sources named %r; there are: %s" % (
                len(hits), name, ", ".join(repr(n) for _, n in names) or "none"))
        self.src = hits[0]
        if cm.MIDIPortConnectSource(self.port.value, self.src, None):
            self.close()
            raise MidiError("coremidi: cannot connect to %r" % name)

    def sources(self):
        cm, cf = self.cm, self.cf
        # kMIDIPropertyName's value: the exported constant can read as NULL through ctypes, and
        # CoreMIDI compares property names by value
        key = cf.CFStringCreateWithCString(None, b"name", K_UTF8)
        out = []
        try:
            for i in range(cm.MIDIGetNumberOfSources()):
                ref = cm.MIDIGetSource(i)
                s = ctypes.c_void_p()
                if cm.MIDIObjectGetStringProperty(ref, key, ctypes.byref(s)) or not s.value:
                    continue
                buf = ctypes.create_string_buffer(256)
                if cf.CFStringGetCString(s, buf, len(buf), K_UTF8):
                    out.append((ref, buf.value.decode("utf-8", "replace")))
                cf.CFRelease(s)
        finally:
            cf.CFRelease(key)
        return out

    def _read(self, pktlist, ref_con, src_con):
        t = time.monotonic_ns()
        for _, data in packets(pktlist):
            self._bytes(data, t)

    def close(self):
        if getattr(self, "src", None) is not None:
            self.cm.MIDIPortDisconnectSource(self.port.value, self.src)
        if self.port.value:
            self.cm.MIDIPortDispose(self.port.value)
        if self.client.value:
            self.cm.MIDIClientDispose(self.client.value)


def open_source(spec):
    kind, sep, arg = spec.partition(":")
    if not sep or not arg:
        raise MidiError("--midi wants raw:PATH or coremidi:NAME, not %r" % spec)
    if kind == "raw":
        return RawSource(arg)
    if kind == "coremidi":
        return CoreMidiSource(arg)
    raise MidiError("--midi: unknown kind %r (raw, coremidi)" % kind)

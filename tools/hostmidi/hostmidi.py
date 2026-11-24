#!/usr/bin/env python3
"""MIDI ports on the host, for the command link over SysEx (docs/link.md, "Over SysEx";
docs/integration.md, "Any OS"): one interface over each OS's own MIDI API through ctypes, standard
library only.

  port = hostmidi.open_port(spec)   # spec: a port's name, fd:N or raw:PATH
  port.send(b"\\xF0 ... \\xF7")      # one whole SysEx
  for msg in port.receive(0.5): ... # whole SysEx messages that arrived (F0 to F7), oldest first
  port.capture()                    # keep channel messages too (make e2e: the performance port)
  port.mark(); ...; port.take()     # [(host monotonic ns, message bytes)] since the mark
  port.close()

Specs:

- NAME      a MIDI port by the name the OS gives it, both directions (the input and the output of
            that name; a name that is part of exactly one port's name is enough): the mixer's
            configuration port is "SMC-Mixer Config". macOS: CoreMIDI. Linux: ALSA rawmidi
            (libasound), the subdevice's name. Windows: WinMM.
- fd:N      an open file descriptor carrying MIDI bytes both ways (a socket: devhost --sysex-fd in
            the tests).
- raw:PATH  a file carrying MIDI bytes both ways (an ALSA rawmidi device /dev/snd/midiCxDy, a
            pseudo-terminal).

Only SysEx is kept from what arrives (receive()): real-time and System Common are dropped; a SysEx
cut short by another status is dropped. Channel messages are dropped too unless capture() was
called: then take() gives them (running status resolved), each with the host's monotonic time in ns
when it was read.

  python3 -I tools/hostmidi/hostmidi.py list      the ports this OS has, inputs and outputs

✅ CoreMIDI has run with the mixer both ways, USB MIDI and BLE MIDI (2026-11-13, make e2e). ⚠️ The
ALSA and WinMM backends are written from their APIs' documentation and have not run; the
byte-stream ports run in the tests (tools/hostmidi/test_hostmidi.py, tools/smcconf/test_device.py).
"""
import ctypes
import ctypes.util
import errno
import os
import platform
import queue
import select
import sys
import threading
import time


class MidiError(Exception):
    pass


class SysexSplitter:
    """A MIDI byte stream to whole SysEx messages."""

    def __init__(self):
        self.buf = None  # bytearray while inside a SysEx

    def feed(self, data):
        out = []
        for b in data:
            if b >= 0xF8:
                continue  # real-time: anywhere
            if b == 0xF0:
                self.buf = bytearray([b])
            elif b == 0xF7:
                if self.buf is not None:
                    self.buf.append(b)
                    out.append(bytes(self.buf))
                self.buf = None
            elif b >= 0x80:
                self.buf = None  # another status ends it unfinished
            elif self.buf is not None:
                self.buf.append(b)
        return out


DATA_BYTES = {0x8: 2, 0x9: 2, 0xA: 2, 0xB: 2, 0xC: 1, 0xD: 1, 0xE: 2}


class ChannelParser:
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


class Port:
    """What every backend gives: send(msg), receive(timeout), capture(), mark(), take(), close().
    Backends call _bytes()."""

    def __init__(self, name):
        self.name = name
        self.split = SysexSplitter()
        self.lock = threading.Lock()
        self.ready = threading.Condition(self.lock)
        self.messages = []
        self.channel = None  # a ChannelParser after capture()
        self.captured = []

    def _bytes(self, data):
        t = time.monotonic_ns()
        msgs = self.split.feed(data)
        with self.ready:
            if self.channel:
                self.captured += [(t, m) for m in self.channel.feed(data)]
            if msgs:
                self.messages += msgs
                self.ready.notify_all()

    def capture(self):
        """Keep channel messages from now on, for take()."""
        with self.lock:
            self.channel, self.captured = ChannelParser(), []
        return self

    def mark(self):
        """Drops the channel messages kept so far."""
        with self.lock:
            self.captured = []

    def take(self):
        """[(host monotonic ns, message bytes)] kept since the last mark() or take()."""
        with self.lock:
            out, self.captured = self.captured, []
        return out

    def receive(self, timeout):
        """The SysEx messages that arrived, waiting up to timeout for the first."""
        with self.ready:
            if not self.messages:
                self.ready.wait(timeout)
            out, self.messages = self.messages, []
        return out

    def send(self, msg):
        raise NotImplementedError

    def close(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class StreamPort(Port):
    """MIDI bytes both ways on a file descriptor, read in a thread."""

    def __init__(self, fd, name, owned):
        super().__init__(name)
        self.fd, self.owned = fd, owned
        os.set_blocking(fd, False)
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, name="hostmidi-in", daemon=True)
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
                return  # the other side closed
            self._bytes(data)

    def send(self, msg):
        data = bytes(msg)
        while data:
            select.select([], [self.fd], [], 1.0)
            try:
                n = os.write(self.fd, data)
            except BlockingIOError:
                continue
            data = data[n:]

    def close(self):
        self.stop.set()
        self.thread.join(1)
        if self.owned:
            os.close(self.fd)


def pick(names, want, what):
    """The one name that is want, or the one that contains it."""
    exact = [n for n in names if n == want]
    hits = exact or [n for n in names if want in n]
    if len(hits) != 1:
        raise MidiError("%s: %d ports match %r; there are: %s" % (
            what, len(hits), want, ", ".join(repr(n) for n in names) or "none"))
    return hits[0]


# --- CoreMIDI (macOS) --------------------------------------------------------------------------

K_UTF8 = 0x08000100  # kCFStringEncodingUTF8
READ_PROC = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)


def coremidi_libs():
    if platform.system() != "Darwin":
        raise MidiError("CoreMIDI: only on macOS")
    paths = [ctypes.util.find_library(n) for n in ("CoreMIDI", "CoreFoundation")]
    if not all(paths):
        raise MidiError("CoreMIDI or CoreFoundation not found")
    cm, cf = ctypes.cdll.LoadLibrary(paths[0]), ctypes.cdll.LoadLibrary(paths[1])
    vp, u32, i32, ul = ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int32, ctypes.c_ulong
    cf.CFStringCreateWithCString.restype = vp
    cf.CFStringCreateWithCString.argtypes = [vp, ctypes.c_char_p, u32]
    cf.CFStringGetCString.restype = ctypes.c_bool
    cf.CFStringGetCString.argtypes = [vp, ctypes.c_char_p, ctypes.c_long, u32]
    cf.CFRelease.argtypes = [vp]
    cm.MIDIClientCreate.restype = i32
    cm.MIDIClientCreate.argtypes = [vp, vp, vp, ctypes.POINTER(u32)]
    cm.MIDIInputPortCreate.restype = i32
    cm.MIDIInputPortCreate.argtypes = [u32, vp, READ_PROC, vp, ctypes.POINTER(u32)]
    cm.MIDIOutputPortCreate.restype = i32
    cm.MIDIOutputPortCreate.argtypes = [u32, vp, ctypes.POINTER(u32)]
    for f in (cm.MIDIGetNumberOfSources, cm.MIDIGetNumberOfDestinations):
        f.restype = ul
    for f in (cm.MIDIGetSource, cm.MIDIGetDestination):
        f.restype = u32
        f.argtypes = [ul]
    cm.MIDIObjectGetStringProperty.restype = i32
    cm.MIDIObjectGetStringProperty.argtypes = [u32, vp, ctypes.POINTER(vp)]
    cm.MIDIPortConnectSource.restype = i32
    cm.MIDIPortConnectSource.argtypes = [u32, u32, vp]
    cm.MIDIPortDisconnectSource.argtypes = [u32, u32]
    cm.MIDISend.restype = i32
    cm.MIDISend.argtypes = [u32, u32, vp]
    cm.MIDIPortDispose.argtypes = [u32]
    cm.MIDIClientDispose.argtypes = [u32]
    return cm, cf


def coremidi_names(cm, cf):
    """Sources and destinations as {"in": [(ref, name)], "out": [(ref, name)]}.

    Prefer kMIDIPropertyDisplayName: a fresh USB device can have short names "" and "Config",
    while its display names are "SMC-Mixer" and "SMC-Mixer Config". Fall back to Name when the
    visible name is absent. Use the property strings: exported constants can be NULL via ctypes.
    """
    keys = [cf.CFStringCreateWithCString(None, value, K_UTF8) for value in (b"displayName", b"name")]
    out = {"in": [], "out": []}
    try:
        for kind, count, get in (("in", cm.MIDIGetNumberOfSources, cm.MIDIGetSource),
                                 ("out", cm.MIDIGetNumberOfDestinations, cm.MIDIGetDestination)):
            for i in range(count()):
                ref = get(i)
                for key in keys:
                    s = ctypes.c_void_p()
                    if cm.MIDIObjectGetStringProperty(ref, key, ctypes.byref(s)) or not s.value:
                        continue
                    try:
                        buf = ctypes.create_string_buffer(256)
                        if cf.CFStringGetCString(s, buf, len(buf), K_UTF8) and buf.value:
                            out[kind].append((ref, buf.value.decode("utf-8", "replace")))
                            break
                    finally:
                        cf.CFRelease(s)
    finally:
        for key in keys:
            cf.CFRelease(key)
    return out


def coremidi_packets(addr):
    """The bytes of each packet of a MIDIPacketList: UInt32 count, then MIDIPacket (UInt64
    timeStamp, UInt16 length, data), packed to 4 bytes on Apple silicon (MIDIPacketNext)."""
    count = ctypes.c_uint32.from_address(addr).value
    off, align, out = addr + 4, 4 if platform.machine() == "arm64" else 1, []
    for _ in range(count):
        n = ctypes.c_uint16.from_address(off + 8).value
        out.append(ctypes.string_at(off + 10, n))
        off = (off + 10 + n + align - 1) // align * align
    return out


class CoreMidiPort(Port):
    def __init__(self, name):
        super().__init__(name)
        self.cm, self.cf = cm, cf = coremidi_libs()
        self.client, self.inp, self.outp, self.src = ctypes.c_uint32(), ctypes.c_uint32(), ctypes.c_uint32(), None
        names = [cf.CFStringCreateWithCString(None, s, K_UTF8) for s in (b"hostmidi", b"in", b"out")]
        try:
            if cm.MIDIClientCreate(names[0], None, None, ctypes.byref(self.client)):
                raise MidiError("CoreMIDI: MIDIClientCreate failed")
            self.proc = READ_PROC(self._read)  # kept: CoreMIDI calls it from its own thread
            if cm.MIDIInputPortCreate(self.client.value, names[1], self.proc, None, ctypes.byref(self.inp)) or \
                    cm.MIDIOutputPortCreate(self.client.value, names[2], ctypes.byref(self.outp)):
                raise MidiError("CoreMIDI: cannot create the ports")
        finally:
            for s in names:
                cf.CFRelease(s)
        try:
            ends = coremidi_names(cm, cf)
            src = pick([n for _, n in ends["in"]], name, "CoreMIDI sources")
            dst = pick([n for _, n in ends["out"]], name, "CoreMIDI destinations")
            self.dst = next(r for r, n in ends["out"] if n == dst)
            src_ref = next(r for r, n in ends["in"] if n == src)
            if cm.MIDIPortConnectSource(self.inp.value, src_ref, None):
                raise MidiError("CoreMIDI: cannot connect to %r" % src)
            self.src = src_ref
        except Exception:
            self.close()
            raise

    def _read(self, pktlist, ref_con, src_con):
        for data in coremidi_packets(pktlist):
            self._bytes(data)

    def send(self, msg):
        msg = bytes(msg)
        buf = ctypes.create_string_buffer(4 + 10 + len(msg) + 4)
        ctypes.c_uint32.from_buffer(buf, 0).value = 1  # one packet, timestamp 0: now
        ctypes.c_uint16.from_buffer(buf, 12).value = len(msg)
        ctypes.memmove(ctypes.addressof(buf) + 14, msg, len(msg))
        if self.cm.MIDISend(self.outp.value, self.dst, ctypes.addressof(buf)):
            raise MidiError("CoreMIDI: MIDISend failed")

    def close(self):
        cm = self.cm
        if self.src is not None:
            cm.MIDIPortDisconnectSource(self.inp.value, self.src)
            self.src = None
        for p in (self.inp, self.outp):
            if p.value:
                cm.MIDIPortDispose(p.value)
                p.value = 0
        if self.client.value:
            cm.MIDIClientDispose(self.client.value)
            self.client.value = 0


def coremidi_list():
    cm, cf = coremidi_libs()
    ends = coremidi_names(cm, cf)
    return [n for _, n in ends["in"]], [n for _, n in ends["out"]]


# --- ALSA rawmidi (Linux) ----------------------------------------------------------------------

SND_RAWMIDI_STREAM_OUTPUT, SND_RAWMIDI_STREAM_INPUT, SND_RAWMIDI_NONBLOCK = 0, 1, 2


def alsa_lib():
    if platform.system() != "Linux":
        raise MidiError("ALSA: only on Linux")
    path = ctypes.util.find_library("asound")
    if not path:
        raise MidiError("ALSA: libasound not found (the alsa-lib package)")
    a = ctypes.cdll.LoadLibrary(path)
    vp, i, pp = ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_void_p)
    a.snd_card_next.argtypes = [ctypes.POINTER(i)]
    a.snd_ctl_open.argtypes = [pp, ctypes.c_char_p, i]
    a.snd_ctl_close.argtypes = [vp]
    a.snd_ctl_rawmidi_next_device.argtypes = [vp, ctypes.POINTER(i)]
    a.snd_ctl_rawmidi_info.argtypes = [vp, vp]
    a.snd_rawmidi_info_malloc.argtypes = [pp]
    a.snd_rawmidi_info_free.argtypes = [vp]
    for f in ("set_device", "set_subdevice", "set_stream"):
        getattr(a, "snd_rawmidi_info_" + f).argtypes = [vp, ctypes.c_uint]
    a.snd_rawmidi_info_get_subdevices_count.argtypes = [vp]
    a.snd_rawmidi_info_get_subdevices_count.restype = ctypes.c_uint
    a.snd_rawmidi_info_get_subdevice_name.argtypes = [vp]
    a.snd_rawmidi_info_get_subdevice_name.restype = ctypes.c_char_p
    a.snd_rawmidi_open.argtypes = [pp, pp, ctypes.c_char_p, i]
    a.snd_rawmidi_close.argtypes = [vp]
    a.snd_rawmidi_read.argtypes = [vp, vp, ctypes.c_size_t]
    a.snd_rawmidi_read.restype = ctypes.c_ssize_t
    a.snd_rawmidi_write.argtypes = [vp, vp, ctypes.c_size_t]
    a.snd_rawmidi_write.restype = ctypes.c_ssize_t
    a.snd_rawmidi_drain.argtypes = [vp]
    return a


def alsa_ports(a):
    """[(hw:C,D,S, subdevice name, has input, has output)]."""
    out, card = [], ctypes.c_int(-1)
    info = ctypes.c_void_p()
    if a.snd_rawmidi_info_malloc(ctypes.byref(info)) < 0:
        raise MidiError("ALSA: out of memory")
    try:
        while a.snd_card_next(ctypes.byref(card)) >= 0 and card.value >= 0:
            ctl = ctypes.c_void_p()
            if a.snd_ctl_open(ctypes.byref(ctl), b"hw:%d" % card.value, 0) < 0:
                continue
            dev = ctypes.c_int(-1)
            while a.snd_ctl_rawmidi_next_device(ctl, ctypes.byref(dev)) >= 0 and dev.value >= 0:
                subs = {}
                for stream in (SND_RAWMIDI_STREAM_INPUT, SND_RAWMIDI_STREAM_OUTPUT):
                    a.snd_rawmidi_info_set_device(info, dev.value)
                    a.snd_rawmidi_info_set_subdevice(info, 0)
                    a.snd_rawmidi_info_set_stream(info, stream)
                    if a.snd_ctl_rawmidi_info(ctl, info) < 0:
                        continue
                    for s in range(a.snd_rawmidi_info_get_subdevices_count(info)):
                        a.snd_rawmidi_info_set_subdevice(info, s)
                        if a.snd_ctl_rawmidi_info(ctl, info) < 0:
                            continue
                        name = (a.snd_rawmidi_info_get_subdevice_name(info) or b"").decode("utf-8", "replace")
                        e = subs.setdefault(s, [name, False, False])
                        e[1 if stream == SND_RAWMIDI_STREAM_INPUT else 2] = True
                for s, (name, i, o) in sorted(subs.items()):
                    out.append(("hw:%d,%d,%d" % (card.value, dev.value, s), name, i, o))
            a.snd_ctl_close(ctl)
    finally:
        a.snd_rawmidi_info_free(info)
    return out


class AlsaPort(Port):
    def __init__(self, name):
        super().__init__(name)
        self.a = a = alsa_lib()
        ports = [p for p in alsa_ports(a) if p[2] and p[3]]
        hw = name if name.startswith("hw:") else None
        if not hw:
            want = pick([p[1] for p in ports], name, "ALSA rawmidi ports")
            hw = next(p[0] for p in ports if p[1] == want)
        self.inp, self.outp = ctypes.c_void_p(), ctypes.c_void_p()
        if a.snd_rawmidi_open(ctypes.byref(self.inp), ctypes.byref(self.outp), hw.encode(), SND_RAWMIDI_NONBLOCK) < 0:
            raise MidiError("ALSA: cannot open %s" % hw)
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, name="hostmidi-in", daemon=True)
        self.thread.start()

    def _run(self):
        buf = ctypes.create_string_buffer(1024)
        while not self.stop.is_set():
            n = self.a.snd_rawmidi_read(self.inp, buf, len(buf))
            if n > 0:
                self._bytes(buf.raw[:n])
            else:
                time.sleep(0.002)  # -EAGAIN: nothing yet

    def send(self, msg):
        data = bytes(msg)
        give_up = time.monotonic() + 2.0
        while data:
            if time.monotonic() >= give_up:
                raise MidiError("ALSA: write timed out after 2 seconds")
            n = self.a.snd_rawmidi_write(self.outp, data, len(data))
            if n > 0:
                data = data[n:]
            elif n < 0 and -n not in (errno.EAGAIN, errno.EINTR):
                raise MidiError("ALSA: write failed: %s (%s)" % (errno.errorcode.get(-n, str(-n)), os.strerror(-n)))
            else:
                time.sleep(0.002)
        # Handles are nonblocking: drain may also return EAGAIN until queued bytes leave.
        while True:
            if time.monotonic() >= give_up:
                raise MidiError("ALSA: drain timed out after 2 seconds")
            n = self.a.snd_rawmidi_drain(self.outp)
            if n >= 0:
                break
            if -n not in (errno.EAGAIN, errno.EINTR):
                raise MidiError("ALSA: drain failed: %s (%s)" % (errno.errorcode.get(-n, str(-n)), os.strerror(-n)))
            time.sleep(0.002)

    def close(self):
        self.stop.set()
        self.thread.join(1)
        for p in (self.inp, self.outp):
            if p.value:
                self.a.snd_rawmidi_close(p)
                p.value = None


def alsa_list():
    ports = alsa_ports(alsa_lib())
    return (["%s (%s)" % (p[1], p[0]) for p in ports if p[2]], ["%s (%s)" % (p[1], p[0]) for p in ports if p[3]])


# --- WinMM (Windows) ---------------------------------------------------------------------------

CALLBACK_FUNCTION, MIM_DATA, MIM_LONGDATA, MHDR_DONE = 0x30000, 0x3C3, 0x3C4, 0x1


class MIDIHDR(ctypes.Structure):
    _fields_ = [("lpData", ctypes.c_void_p), ("dwBufferLength", ctypes.c_uint32),
                ("dwBytesRecorded", ctypes.c_uint32), ("dwUser", ctypes.c_size_t), ("dwFlags", ctypes.c_uint32),
                ("lpNext", ctypes.c_void_p), ("reserved", ctypes.c_size_t), ("dwOffset", ctypes.c_uint32),
                ("dwReserved", ctypes.c_size_t * 8)]


class MIDIINCAPSW(ctypes.Structure):
    _fields_ = [("wMid", ctypes.c_uint16), ("wPid", ctypes.c_uint16), ("vDriverVersion", ctypes.c_uint32),
                ("szPname", ctypes.c_wchar * 32), ("dwSupport", ctypes.c_uint32)]


class MIDIOUTCAPSW(ctypes.Structure):
    _fields_ = [("wMid", ctypes.c_uint16), ("wPid", ctypes.c_uint16), ("vDriverVersion", ctypes.c_uint32),
                ("szPname", ctypes.c_wchar * 32), ("wTechnology", ctypes.c_uint16), ("wVoices", ctypes.c_uint16),
                ("wNotes", ctypes.c_uint16), ("wChannelMask", ctypes.c_uint16), ("dwSupport", ctypes.c_uint32)]


def winmm_lib():
    if platform.system() != "Windows":
        raise MidiError("WinMM: only on Windows")
    return ctypes.WinDLL("winmm")


def winmm_names(w):
    ins, outs = [], []
    for i in range(w.midiInGetNumDevs()):
        c = MIDIINCAPSW()
        if w.midiInGetDevCapsW(i, ctypes.byref(c), ctypes.sizeof(c)) == 0:
            ins.append(c.szPname)
    for i in range(w.midiOutGetNumDevs()):
        c = MIDIOUTCAPSW()
        if w.midiOutGetDevCapsW(i, ctypes.byref(c), ctypes.sizeof(c)) == 0:
            outs.append(c.szPname)
    return ins, outs


class WinMmPort(Port):
    BUFFERS, SIZE = 4, 1024

    def __init__(self, name):
        super().__init__(name)
        self.w = w = winmm_lib()
        ins, outs = winmm_names(w)
        self.hin, self.hout = ctypes.c_void_p(), ctypes.c_void_p()
        self.done = queue.SimpleQueue()  # short messages and filled headers, in callback order
        self.proc = ctypes.WINFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_size_t,
                                       ctypes.c_size_t)(self._callback)
        if w.midiInOpen(ctypes.byref(self.hin), ins.index(pick(ins, name, "WinMM inputs")), self.proc, 0,
                        CALLBACK_FUNCTION):
            raise MidiError("WinMM: cannot open the input %r" % name)
        if w.midiOutOpen(ctypes.byref(self.hout), outs.index(pick(outs, name, "WinMM outputs")), 0, 0, 0):
            w.midiInClose(self.hin)
            raise MidiError("WinMM: cannot open the output %r" % name)
        self.headers = []
        for _ in range(self.BUFFERS):
            data = ctypes.create_string_buffer(self.SIZE)
            h = MIDIHDR(lpData=ctypes.addressof(data), dwBufferLength=self.SIZE)
            self.headers.append((h, data))
            w.midiInPrepareHeader(self.hin, ctypes.byref(h), ctypes.sizeof(h))
            w.midiInAddBuffer(self.hin, ctypes.byref(h), ctypes.sizeof(h))
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, name="hostmidi-in", daemon=True)
        self.thread.start()
        w.midiInStart(self.hin)

    def _callback(self, h, msg, inst, p1, p2):
        if msg in (MIM_DATA, MIM_LONGDATA):  # no native calls here: the worker parses/re-adds
            self.done.put((msg, p1))

    def _run(self):
        by_addr = {ctypes.addressof(h): h for h, _ in self.headers}
        while not self.stop.is_set():
            try:
                msg, value = self.done.get(timeout=0.05)
            except queue.Empty:
                continue
            if msg == MIM_DATA:
                status = value & 0xFF
                count = DATA_BYTES.get(status >> 4) if 0x80 <= status < 0xF0 else None
                if count is not None:
                    self._bytes(bytes((value >> (8 * i)) & 0xFF for i in range(count + 1)))
                continue
            h = by_addr.get(value)
            if h is None:
                continue
            if h.dwBytesRecorded:
                self._bytes(ctypes.string_at(h.lpData, h.dwBytesRecorded))
            if not self.stop.is_set():
                self.w.midiInAddBuffer(self.hin, ctypes.byref(h), ctypes.sizeof(h))

    def send(self, msg):
        data = ctypes.create_string_buffer(bytes(msg), len(msg))
        h = MIDIHDR(lpData=ctypes.addressof(data), dwBufferLength=len(msg))
        w = self.w
        if w.midiOutPrepareHeader(self.hout, ctypes.byref(h), ctypes.sizeof(h)) or \
                w.midiOutLongMsg(self.hout, ctypes.byref(h), ctypes.sizeof(h)):
            raise MidiError("WinMM: midiOutLongMsg failed")
        give_up = time.monotonic() + 2.0
        while not h.dwFlags & MHDR_DONE and time.monotonic() < give_up:
            time.sleep(0.001)
        w.midiOutUnprepareHeader(self.hout, ctypes.byref(h), ctypes.sizeof(h))

    def close(self):
        if not self.hin.value:
            return
        self.stop.set()
        w = self.w
        w.midiInStop(self.hin)
        w.midiInReset(self.hin)  # returns the buffers
        self.thread.join(1)
        for h, _ in self.headers:
            w.midiInUnprepareHeader(self.hin, ctypes.byref(h), ctypes.sizeof(h))
        w.midiInClose(self.hin)
        w.midiOutClose(self.hout)
        self.hin.value = None


def winmm_list():
    return winmm_names(winmm_lib())


# --- choosing ----------------------------------------------------------------------------------

BACKENDS = {"Darwin": (CoreMidiPort, coremidi_list), "Linux": (AlsaPort, alsa_list),
            "Windows": (WinMmPort, winmm_list)}


def open_port(spec):
    """A port by its spec (the module's help)."""
    kind, sep, arg = spec.partition(":")
    if sep and kind == "fd" and arg.isdigit():
        return StreamPort(int(arg), spec, owned=False)
    if sep and kind == "raw" and arg:
        try:
            fd = os.open(arg, os.O_RDWR | getattr(os, "O_NOCTTY", 0))
        except OSError as e:
            raise MidiError("cannot open %s: %s" % (arg, e))
        if os.isatty(fd):
            import tty
            tty.setraw(fd)
        return StreamPort(fd, spec, owned=True)
    backend = BACKENDS.get(platform.system())
    if not backend:
        raise MidiError("no MIDI backend for %s: use fd:N or raw:PATH" % platform.system())
    return backend[0](spec)


def list_ports():
    """(inputs, outputs): the names this OS's backend sees."""
    backend = BACKENDS.get(platform.system())
    if not backend:
        raise MidiError("no MIDI backend for %s" % platform.system())
    return backend[1]()


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv != ["list"]:
        print(__doc__.split("\n\n")[0] + "\n\nusage: hostmidi.py list", file=sys.stderr)
        return 2
    try:
        ins, outs = list_ports()
    except MidiError as e:
        print("hostmidi: %s" % e, file=sys.stderr)
        return 1
    for title, names in (("inputs", ins), ("outputs", outs)):
        print("%s:" % title)
        for n in names:
            print("  %s" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())

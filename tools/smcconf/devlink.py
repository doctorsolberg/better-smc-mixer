"""The configuration protocol's host side (docs/link.md, "Configuration"; SPEC §7): reads the
device's running configuration and replaces it, as its binary form, over the command link
(tools/smclog/smclog.py's Link: frames, retries with the same seq, FR-CFG-03): on the USB CDC port,
or over SysEx on a MIDI port (SysexLink, tools/hostmidi; docs/link.md, "Over SysEx").

Standard library only. smcconf.py's pull, push and info use it; tools/smcconf/test_device.py tests
it against build/host/devhost.
"""
import os
import struct
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "smclog"))
sys.path.insert(0, os.path.join(HERE, "..", "hostmidi"))
import hostmidi  # noqa: E402
import smclog  # noqa: E402

VERSION = 4  # the configuration protocol this tool speaks (CFGLINK_VERSION)
CMD = smclog.CFG_CMDS
STATUS = {0: "ok", 1: "busy", 2: "no transfer", 3: "bad offset", 4: "size", 5: "incomplete", 6: "bad CRC",
          7: "invalid", 8: "unavailable"}
STATE = {0: "idle", 1: "get", 2: "put", 3: "applying", 4: "saving"}
SOURCE = {0: "factory", 1: "test", 2: "received", 3: "stored"}
STORE = {0: "none", 1: "clean", 2: "waiting", 3: "writing", 4: "failed", 5: "unsaved"}  # enum cfglink_store
SOURCE_SETTINGS = 0x80
CFGBIN_ERRORS = ["ok", "short", "magic", "version", "length", "crc", "record", "tag", "twice", "order", "where",
                 "field", "type", "value", "shift", "space"]  # cfgbin.h, enum cfgbin_code
BUSY_WAIT_S = 2.0  # how long a busy device is asked again
SAVE_WAIT_S = 30.0  # how long wait_saved() waits (the autosave comes 3 s after the last change and
                    # the last touch of a control; a save pauses while the panel is in use)


STATE_LEN = 30  # the panel's state (docs/link.md, "The panel's state"; CFGLINK_STATE_LEN)
BT = {0: "off", 1: "on", 2: "none"}
ROW_PAGE = 255  # the binary form's page of the bottom row
SENT_LEN = 7  # an entry of what the mixer sent (docs/link.md, "What the mixer sent"; CFGLINK_SENT_LEN)
SENT_NOW = 0xFFFFFFFF  # CFG_SENT's from: none, and the next number


class DeviceError(Exception):
    pass


def parse_state(p):
    """The panel's state (CFG_STATE's reply after its status, an EVENT's after its count) as a dict:
    preset and page 0-based, the last touch as (preset, page, control, layer) in the binary form's
    address (page 255: the bottom row), None before the first."""
    if len(p) < STATE_LEN:
        raise DeviceError("the panel's state is %d bytes, not %d" % (len(p), STATE_LEN))
    preset, page, shift, bt, touches, tp, tg, tc, tl, store = struct.unpack_from("<10B", p)
    revision, = struct.unpack_from("<I", p, 10)
    ident = bytes(p[14:30])
    return {"preset": preset, "page": page, "shift": bool(shift), "bluetooth": BT.get(bt & 0x7F, str(bt & 0x7F)),
            "central": bool(bt & 0x80), "touches": touches,
            "touched": None if tp == 255 else {"preset": tp, "page": tg, "control": tc, "layer": tl},
            "store": STORE.get(store, str(store)), "revision": revision,
            "id": None if ident == bytes(16) else ident}


def message_len(status):
    """A channel message's length by its status byte."""
    return 2 if status & 0xF0 in (0xC0, 0xD0) else 3


def parse_entries(p, first):
    """What the mixer sent (P-5): entries of SENT_LEN bytes as dicts: number n, the message's bytes,
    and the control that sent it as (preset, page, control, layer) in the binary form's address,
    None when no control did (MIDI in, the time, a selection)."""
    out = []
    for i in range(len(p) // SENT_LEN):
        e = p[i * SENT_LEN:(i + 1) * SENT_LEN]
        preset, page, control, layer = e[3:7]
        out.append({"n": (first + i) & 0xFFFFFFFF, "msg": bytes(e[:message_len(e[0])]),
                    "source": None if control == 255 else
                    {"preset": preset, "page": page, "control": control, "layer": layer}})
    return out


def source_name(v):
    name = SOURCE.get(v & 0x7F, str(v & 0x7F))
    return name + (" with settings" if v & SOURCE_SETTINGS else "")


def error_text(p):
    """cfgbin's error after an INVALID status: code, preset, page, control, field, offset."""
    if len(p) < 9:
        return "not valid"
    code, preset, page, control, field, offset = struct.unpack_from("<BBBBBI", p)
    where = ["byte %d" % offset]
    if preset != 255:
        where.append("preset %d" % (preset + 1))
    if page != 255:
        where.append("page %d" % (page + 1))
    if control != 255:
        where.append("control %d" % control)
    if field != 255:
        where.append("field %d" % field)
    name = CFGBIN_ERRORS[code] if code < len(CFGBIN_ERRORS) else str(code)
    return "%s (%s)" % (name, ", ".join(where))


class Device:
    """The configuration protocol on a serial port (port) or a MIDI port (midi: hostmidi's spec)."""

    def __init__(self, port=None, timeout=1.0, midi=None):
        self.fd, self.midi = None, None
        if midi is not None:
            self.midi = hostmidi.open_port(midi)
            self.link = smclog.SysexLink(self.midi)
        else:
            self.fd = smclog.open_serial(port)
            self.link = smclog.Link(self.fd, smclog.Decoder({"sites": {}}))
        self.timeout = timeout
        self._backlog = []  # EVENT and SENT records polled for the other kind's call (_take())

    def close(self):
        if self.midi is not None:
            self.midi.close()
        else:
            os.close(self.fd)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def call(self, name, payload=b""):
        """One CFG command; returns (status name, the payload after the status byte). A busy device
        is asked again (with a new seq) for up to BUSY_WAIT_S."""
        give_up = time.monotonic() + BUSY_WAIT_S
        while True:
            try:
                r = self.link.call(CMD[name], payload, timeout=self.timeout)
            except smclog.SmclogError as e:
                raise DeviceError(str(e))
            if r["status"] != "ok":
                raise DeviceError("%s: %s" % (name, r["status"]))
            p = bytes.fromhex(r["payload"])
            if not p:
                raise DeviceError("%s: an empty reply" % name)
            status = STATUS.get(p[0], str(p[0]))
            if status != "busy" or time.monotonic() > give_up:
                return status, p[1:]
            time.sleep(0.02)

    def link_info(self):
        try:
            return self.link.call(smclog.CMDS["info"], b"", timeout=self.timeout)
        except smclog.SmclogError as e:
            raise DeviceError(str(e))

    def info(self):
        """CFG_INFO as a dict; DeviceError if the device has no configuration protocol."""
        li = self.link_info()
        if li["status"] != "ok" or "config" not in li.get("caps", []):
            raise DeviceError("the device has no configuration protocol (capabilities: %s)"
                              % ",".join(li.get("caps", [])))
        status, p = self.call("cfg-info")
        if status != "ok" or len(p) < 31:
            raise DeviceError("cfg-info: %s" % status)
        version, binary, schema, max_len, chunk, state, source, revision = struct.unpack_from("<BBHIBBBI", p)
        ident = p[15:31]
        store, generation = struct.unpack_from("<BI", p, 31) if len(p) >= 36 else (0, 0)
        return {"version": version, "binary_version": binary, "schema_version": schema, "max_length": max_len,
                "chunk": chunk, "state": STATE.get(state, str(state)), "source": source_name(source),
                "revision": revision, "id": None if ident == bytes(16) else ident,
                "store": STORE.get(store, str(store)), "generation": generation,
                "build_id": li.get("build_id")}

    def wait_saved(self, after_generation, timeout=SAVE_WAIT_S):
        """Waits until the device has saved a configuration above after_generation; returns CFG_INFO.
        DeviceError if it has no storage, gives up, or does not within timeout."""
        give_up = time.monotonic() + timeout
        while True:
            info = self.info()
            if info["store"] == "none":
                raise DeviceError("the device has no flash storage for its configuration")
            if info["store"] == "failed":
                raise DeviceError("the device could not save the configuration (the one saved before stays)")
            if info["store"] == "clean" and info["generation"] > after_generation:
                return info
            if info["store"] == "clean" and info["source"] != "received":
                raise DeviceError("the configuration was replaced before it was saved (%s)" % info["source"])
            if info["store"] == "unsaved":
                raise DeviceError("the device saves when told to (its setting save is manual): use --save")
            if time.monotonic() > give_up:
                raise DeviceError("not saved within %.0f s (store %s): is the panel in use?"
                                  % (timeout, info["store"]))
            time.sleep(0.1)

    def check_compatible(self, info, binary_version, schema_version):
        """FR-CFG-07: refuse a device that speaks another protocol, binary form or schema."""
        if info["version"] != VERSION:
            raise DeviceError("the device speaks configuration protocol %d, this tool %d"
                              % (info["version"], VERSION))
        if (info["binary_version"], info["schema_version"]) != (binary_version, schema_version):
            raise DeviceError("the device takes binary form %d, schema %d; this tool writes %d, %d: update the "
                              "firmware or the tools" % (info["binary_version"], info["schema_version"],
                                                         binary_version, schema_version))

    def pull(self, chunk=128):
        """The running configuration's binary form (a snapshot), checked by its CRC-32."""
        status, p = self.call("cfg-get-begin")
        if status != "ok":
            raise DeviceError("cfg-get-begin: %s" % (error_text(p) if status == "invalid" else status))
        length, crc = struct.unpack_from("<II", p)
        out = bytearray()
        while len(out) < length:
            status, p = self.call("cfg-get-chunk", struct.pack("<IB", len(out), min(chunk, 255)))
            if status != "ok":
                raise DeviceError("cfg-get-chunk at %d: %s" % (len(out), status))
            at, = struct.unpack_from("<I", p)
            data = p[4:]
            if at != len(out) or not data:
                raise DeviceError("cfg-get-chunk at %d: %d bytes at %d" % (len(out), len(data), at))
            out += data
        if len(out) != length or zlib.crc32(out) != crc:
            raise DeviceError("the configuration read does not check (%d bytes, CRC-32 %08x, want %d, %08x)"
                              % (len(out), zlib.crc32(out), length, crc))
        return bytes(out)

    def push(self, blob, chunk=128):
        """Uploads blob and commits it; returns once the device has applied it. Nothing changes on
        the device unless every check passes there (length, CRC-32, the whole form)."""
        status, _ = self.call("cfg-put-begin", struct.pack("<II", len(blob), zlib.crc32(blob)))
        if status != "ok":
            raise DeviceError("cfg-put-begin (%d bytes): %s" % (len(blob), status))
        at = 0
        while at < len(blob):
            status, p = self.call("cfg-put-chunk", struct.pack("<I", at) + blob[at:at + chunk])
            got, = struct.unpack_from("<I", p) if len(p) >= 4 else (None,)
            if status == "bad offset" and got is not None and got <= len(blob):
                at = got  # the device says where it stands (a reply lost on the way)
                continue
            if status != "ok":
                self.abort()
                raise DeviceError("cfg-put-chunk at %d: %s" % (at, status))
            at = got
        status, p = self.call("cfg-put-commit")
        if status != "ok":
            raise DeviceError("cfg-put-commit: %s" % (error_text(p) if status == "invalid" else status))
        give_up = time.monotonic() + BUSY_WAIT_S
        while True:
            info = self.info()
            if info["state"] != "applying":
                return info
            if time.monotonic() > give_up:
                raise DeviceError("the device has not applied the configuration within %.0f s" % BUSY_WAIT_S)
            time.sleep(0.02)

    def set(self, revision, preset, records):
        """CFG_SET (P-1): records of the binary form for one preset (codec.edits()), applied by the
        device without a restart. Revision is a legacy wire value; the reply acknowledges receipt."""
        status, p = self.call("cfg-set", struct.pack("<IB", revision, preset) + records)
        if status != "ok":
            raise DeviceError("cfg-set (preset %d, %d bytes): %s" % (
                preset + 1, len(records), error_text(p) if status == "invalid" else status))

    def edit(self, edits, revision):
        """Every live edit of codec.edits() in order; returns CFG_INFO once the last is applied."""
        if not edits:
            return self.info()
        for preset, records in edits:
            self.set(revision, preset, records)
        give_up = time.monotonic() + BUSY_WAIT_S
        while True:
            info = self.info()
            # Old firmware could report idle with zero metadata while a SET was pending.
            # An applied SET always has source received (or test, for a test setup).
            if info["state"] != "applying" and info["source"] in ("received", "test", "test with settings"):
                return info
            if time.monotonic() > give_up:
                raise DeviceError("the device has not applied the edits within %.0f s" % BUSY_WAIT_S)
            time.sleep(0.02)

    def save(self, timeout=SAVE_WAIT_S):
        """CFG_SAVE (Q-26): the running configuration saved now; returns CFG_INFO once it is (at
        once when nothing differs from the saved one)."""
        before = self.info()
        status, _ = self.call("cfg-save")
        if status == "unavailable":
            raise DeviceError("nothing to save to: no flash storage, or the running configuration is a "
                              "setup's (%s)" % before["source"])
        if status != "ok":
            raise DeviceError("cfg-save: %s" % status)
        give_up = time.monotonic() + timeout
        while True:
            info = self.info()
            if info["store"] == "failed":
                raise DeviceError("the device could not save the configuration (the one saved before stays)")
            if info["store"] == "clean" and (info["generation"] > before["generation"] or before["store"] == "clean"):
                return info
            if time.monotonic() > give_up:
                raise DeviceError("not saved within %.0f s (store %s): is the panel in use?" % (timeout, info["store"]))
            time.sleep(0.05)

    def revert(self):
        """CFG_REVERT (Q-26): what the device starts with (its saved configuration, or the factory
        one) replaces the running one; returns CFG_INFO once applied."""
        status, _ = self.call("cfg-revert")
        if status != "ok":
            raise DeviceError("cfg-revert: %s" % status)
        give_up = time.monotonic() + BUSY_WAIT_S
        while True:
            info = self.info()
            if info["state"] != "applying":
                return info
            if time.monotonic() > give_up:
                raise DeviceError("the device has not reverted within %.0f s" % BUSY_WAIT_S)
            time.sleep(0.02)

    def state(self):
        """CFG_STATE (P-2, P-6): the panel's state, parse_state()'s dict."""
        status, p = self.call("cfg-state")
        if status != "ok":
            raise DeviceError("cfg-state: %s" % status)
        return parse_state(p)

    def select(self, preset=None, page=None):
        """CFG_SELECT (P-3): the device switches to preset and page (0-based; None keeps one, a preset
        alone goes to its last page) in its next round."""
        arg = struct.pack("<BB", 0xFF if preset is None else preset, 0xFF if page is None else page)
        try:
            r = self.link.call(CMD["cfg-select"], arg, timeout=self.timeout)
        except smclog.SmclogError as e:
            raise DeviceError(str(e))
        if r["status"] != "ok":
            raise DeviceError("cfg-select (preset %s, page %s): %s" % (preset, page, r["status"]))
        status = STATUS.get(bytes.fromhex(r["payload"])[:1][0], "?") if r["payload"] else "an empty reply"
        if status != "ok":
            raise DeviceError("cfg-select: %s" % status)

    def watch(self, on=True, sent=False):
        """EVENTS (P-2, P-5): the panel's EVENTs on or off, and what the mixer sent; then events()
        and sent() give them as they come."""
        what = (1 if on else 0) | (2 if sent else 0)
        try:
            r = self.link.call(smclog.CMDS["events"], bytes([what]), timeout=self.timeout)
        except smclog.SmclogError as e:
            raise DeviceError(str(e))
        if r["status"] != "ok":
            raise DeviceError("events %d: %s" % (what, r["status"]))

    def _take(self, kind, timeout):
        """The records of kind ("event" or "sent") that came, waiting up to timeout for the first;
        the other kind's are kept for their own call."""
        backlog = self._backlog
        deadline = time.monotonic() + timeout
        while True:
            backlog.extend(self.link.poll(0 if any(r["kind"] == kind for r in backlog)
                                          else max(0.0, deadline - time.monotonic())))
            mine = [r for r in backlog if r["kind"] == kind]
            if mine or time.monotonic() >= deadline:
                backlog[:] = [r for r in backlog if r["kind"] != kind]
                return mine

    def events(self, timeout):
        """The EVENTs that came, waiting up to timeout for the first: [(count, parse_state())]."""
        return [(e["count"], parse_state(bytes.fromhex(e["state"]))) for e in self._take("event", timeout)]

    def sent(self, timeout):
        """What the mixer sent (SENT records, P-5) since the last call, waiting up to timeout for the
        first: (entries lost, [parse_entries()' dicts])."""
        lost, out = 0, []
        for r in self._take("sent", timeout):
            if "bad" in r:
                raise DeviceError("a SENT record that does not parse: %s" % r["bad"])
            lost += r["lost"]
            out.extend(parse_entries(b"".join(bytes.fromhex(e) for e in r["entries"]), r["first"]))
        return lost, out

    def sent_since(self, number=SENT_NOW):
        """CFG_SENT (P-5): what the mixer sent from that number on, at most 16 entries: (first,
        [parse_entries()' dicts]); first is the next number when there is none."""
        status, p = self.call("cfg-sent", struct.pack("<I", number))
        if status != "ok":
            raise DeviceError("cfg-sent: %s" % status)
        if len(p) < 5 or len(p) != 5 + p[4] * SENT_LEN:
            raise DeviceError("cfg-sent: a reply of %d bytes" % len(p))
        first, = struct.unpack_from("<I", p)
        return first, parse_entries(p[5:], first)

    def abort(self):
        try:
            self.call("cfg-abort")
        except DeviceError:
            pass

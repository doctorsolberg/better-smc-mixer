#!/usr/bin/env python3
"""Transcripts of the command link over SysEx (docs/integration.md, T-2; docs/link.md, "Over SysEx"):
a scripted session with the device's own link and configuration protocol code (build/host/devhost
--sysex-fd), every request and reply as the bytes on the wire and as their fields. The editor's link
client (app/editor/js/link.js, I-3) must make the same request bytes from the same fields and
read the same fields from the replies; tools/editortest/run.mjs checks it in Node.

  python3 -I tools/editortest/transcripts.py            check: devhost answers the session as the
                                                        kept transcript says (exit 1 if not)
  python3 -I tools/editortest/transcripts.py --write    record it again

One file per link version: vectors/link-v1.json (the link's version 1; the configuration protocol's
version, 3 since STATE, SELECT and the events, is in its CFG_INFO replies). An exchange after
EVENTS on lists the EVENTs that followed its reply ("events": each message, its count and the
panel's state). Every exchange is deterministic
(no PING, whose reply carries the device's clock, and no GET_INFO, whose reply carries the build's
ID; the session waits for the device to apply a push, an edit or a revert, and to save, instead of
polling). devhost runs with flash (a fresh file), so SAVE and REVERT have a store, and with --boot (a
synthetic installed flash, firmware/tests/bootsel_vec.py), so STOCK checks and goes back. Standard library only; needs build/host/devhost (make -C firmware sim).
"""
import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "smcconf"))
import codec  # noqa: E402
import devlink  # noqa: E402

smclog = devlink.smclog
DEVHOST = os.environ.get("SMC_DEVHOST") or os.path.join(ROOT, "build", "host", "devhost")
OUT = os.path.join(ROOT, "tools", "editortest", "vectors", "link-v1.json")
FIRST_SEQ = 0xF0  # the session's first seq: the transcript crosses its wrap to 0
CHUNK = 128
PUSHED = os.path.join(ROOT, "config", "test.json")


def session(port):
    """The exchanges: [{seq, cmd, payload, request, replies, status, reply_payload}] (hex)."""
    out = []
    seq = [FIRST_SEQ - 1]

    def call(cmd, payload=b"", wait=1.0, events=0, sent=0):
        """One exchange; events, sent: how many EVENTs and SENT records follow its reply (EVENTS
        on), waited for; kept apart, each in its order (which of the two comes first is timing)."""
        seq[0] = (seq[0] + 1) & 0xFF
        req = smclog.sysex_command(seq[0], cmd, payload)
        port.send(req)
        got = port.receive(wait)
        while not got:
            raise SystemExit("transcripts: no reply to %02x" % cmd)
        give_up = time.monotonic() + 1.0
        while len(got) < 1 + events + sent and time.monotonic() < give_up:
            got += port.receive(0.1)
        got += port.receive(0.1)  # nothing more may come
        rec = smclog.sysex_record(got[0])
        if rec is None or rec[0] != smclog.KIND_REPLY or len(got) != 1 + events + sent:
            raise SystemExit("transcripts: an unexpected reply to %02x: %s" % (cmd, [m.hex() for m in got]))
        e = {"seq": seq[0], "cmd": smclog.CMD_NAMES.get(cmd, "0x%02x" % cmd), "payload": payload.hex(),
             "request": req.hex(), "reply": got[0].hex(), "reply_seq": rec[1], "reply_cmd": rec[2],
             "status": rec[3], "reply_payload": rec[4:].hex()}
        after = [(m, smclog.sysex_record(m)) for m in got[1:]]
        evs = [(m, x) for m, x in after if x is not None and x[0] == smclog.KIND_EVENT]
        sns = [(m, x) for m, x in after if x is not None and x[0] == smclog.KIND_SENT]
        if len(evs) != events or len(sns) != sent:
            raise SystemExit("transcripts: not %d EVENTs and %d SENT after %02x: %s"
                             % (events, sent, cmd, [m.hex() for m in got[1:]]))
        if events:
            e["events"] = [{"message": m.hex(), "count": x[1], "state": x[2:].hex()} for m, x in evs]
        if sent:
            e["sent"] = [{"message": m.hex(), "first": struct.unpack_from("<I", x, 1)[0], "lost": x[5],
                          "entries": [x[7 + 7 * i:14 + 7 * i].hex() for i in range(x[6])]} for m, x in sns]
        out.append(e)
        return rec[3], rec[4:]

    C = smclog.CFG_CMDS
    call(C["cfg-info"])
    # pull the running (factory) configuration
    st, p = call(C["cfg-get-begin"])
    length, _ = struct.unpack_from("<II", p, 1)
    at = 0
    while at < length:
        st, p = call(C["cfg-get-chunk"], struct.pack("<IB", at, CHUNK))
        at += len(p) - 5
    call(C["cfg-abort"])
    # push the test configuration with an ID and a revision
    doc = codec.load(PUSHED)
    doc["id"] = bytes.fromhex("0f1e2d3c4b5a49788695a4b3c2d1e0f9")
    doc["revision"] = 5
    blob = codec.encode(doc)
    call(C["cfg-put-begin"], struct.pack("<II", len(blob), zlib.crc32(blob)))
    for at in range(0, len(blob), CHUNK):
        call(C["cfg-put-chunk"], struct.pack("<I", at) + blob[at:at + CHUNK])
    call(C["cfg-put-commit"])
    time.sleep(0.3)  # applied between two rounds of the control task
    call(C["cfg-info"])
    # saved by hand (Q-26): SAVE, then the state once written
    call(C["cfg-save"])
    time.sleep(1.0)
    call(C["cfg-info"])
    # A live edit (P-1): page 2's encoder 1 to CC 77, with legacy revision 6; then one refused.
    edited = codec.load(PUSHED)
    edited["presets"][0]["pages"][1]["strips"][0]["encoder"]["normal"]["number"] = 77
    (preset, records), = codec.edits(doc, edited)
    call(C["cfg-set"], struct.pack("<IB", 6, preset) + records)
    time.sleep(0.1)
    call(C["cfg-info"])
    call(C["cfg-set"], struct.pack("<IB", 7, 4) + records)
    # back to what it starts with: the saved one
    call(C["cfg-revert"])
    time.sleep(0.3)
    call(C["cfg-info"])
    # the panel (P-2, P-3, P-6): its state, the events (the first at once), a selection, a touch;
    # what the mixer sent, with the control (P-5)
    call(C["cfg-state"])
    call(C["cfg-sent"], struct.pack("<I", 0xFFFFFFFF))
    call(smclog.CMDS["events"], b"\x03", events=1)
    call(C["cfg-select"], b"\x02\x01", events=1)  # preset 3 page 2
    call(C["cfg-select"], b"\xff\x03", events=1)  # the page alone: page 4
    call(smclog.CMDS["inject"], b"\x01")
    call(smclog.CMDS["inject"], b"\x02" + bytes([2, 5]) + struct.pack("<h", -1) + b"\x00", events=1, sent=1)  # encoder 6
    call(smclog.CMDS["inject"], b"\x02" + bytes([1, 32 + 3, 1, 0, 0]), events=1, sent=1)  # the row's Rew pressed
    call(smclog.CMDS["inject"], b"\x02" + bytes([1, 32 + 3, 0, 0, 0]), sent=1)  # released: no touch, a message
    call(C["cfg-sent"], struct.pack("<I", 0))  # the same three from the start, by CFG_SENT
    call(smclog.CMDS["inject"], b"\x00")
    call(C["cfg-select"], b"\x04\x00")  # out of range
    call(smclog.CMDS["events"], b"\x00")
    call(C["cfg-state"])
    # refusals: a chunk with no transfer, a command the device does not know, the log stream
    call(C["cfg-put-chunk"], struct.pack("<I", 0) + b"\x00")
    call(0x7F)
    call(smclog.CMDS["subscribe"])
    # going back to M-Vave's firmware: the check, a wrong magic, then back (the device resets after it)
    call(smclog.CMDS["stock"], b"\x00")
    call(smclog.CMDS["stock"], b"\x01BACX")
    call(smclog.CMDS["stock"], b"\x01BACK")
    return out


def record():
    host, dev = socket.socketpair()
    master, slave = os.openpty()
    tmp = tempfile.TemporaryDirectory()
    boot = os.path.join(tmp.name, "boot.bin")
    subprocess.run([sys.executable, "-I", os.path.join(ROOT, "firmware", "tests", "bootsel_vec.py"), "--installed", boot],
                   check=True)
    # The 158-exchange session can take over 40 s on macOS; allow headroom as live.py does.
    proc = subprocess.Popen([DEVHOST, "120", "--sysex-fd", str(dev.fileno()), "--flash",
                             os.path.join(tmp.name, "flash.bin"), "--boot", boot], stdin=master, stdout=master,
                            pass_fds=(dev.fileno(),))
    dev.close()
    try:
        time.sleep(0.1)
        with devlink.hostmidi.open_port("fd:%d" % host.fileno()) as port:
            ex = session(port)
    finally:
        proc.kill()
        proc.wait()
        host.close()
        os.close(master)
        os.close(slave)
        tmp.cleanup()
    about = {"made_by": "tools/editortest/transcripts.py", "link_version": 1, "cfg_version": 4, "transport": "sysex",
             "frame": "F0 7D 53 4D, the record and its CRC-16/CCITT-FALSE (little-endian) 7-bit packed, F7",
             "exchanges": len(ex)}
    return '{"about": %s,\n"exchanges": [\n%s\n]}\n' % (
        json.dumps(about), ",\n".join(json.dumps(e, separators=(",", ":")) for e in ex))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--write", action="store_true")
    a = ap.parse_args(argv)
    if not os.access(DEVHOST, os.X_OK):
        print("transcripts.py: no %s: make -C firmware sim" % DEVHOST, file=sys.stderr)
        return 2
    text = record()
    if a.write:
        with open(OUT, "w", encoding="utf-8") as f:
            f.write(text)
        print("wrote %s" % os.path.relpath(OUT, ROOT))
        return 0
    try:
        with open(OUT, encoding="utf-8") as f:
            old = f.read()
    except FileNotFoundError:
        old = None
    if old != text:
        print("stale transcript %s: the device's link answers otherwise now. If the change is meant, record it "
              "again (python3 -I tools/editortest/transcripts.py --write); then the editor's client must pass "
              "on it (tools/env/test_editor.sh)" % os.path.relpath(OUT, ROOT))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""Tests for smclog.py. Run: python3 -I tools/smclog/test_smclog.py

The integration tests use build/host/test_smclog (built by make -C firmware test): its dictionary
(Mach-O), the stream it emits with --emit, and a capture through a pseudo-terminal. The command
link tests run build/host/test_link --serve, the firmware's link code on the master side of a
pseudo-terminal, against 'cmd' and 'capture'. The metrics tests read the heartbeat of
build/host/devhost (the controller's portable code) through the link; the boot journal tests run it
twice on one keep file, with a simulated crash in between. Without those binaries the tests are
skipped.
"""
import json
import os
import random
import signal
import struct
import subprocess
import sys
import tempfile
import time
import types
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import smclog as sl  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
TEST_BIN = os.path.join(ROOT, "build", "host", "test_smclog")
LINK_BIN = os.path.join(ROOT, "build", "host", "test_link")
DEVHOST = os.path.join(ROOT, "build", "host", "devhost")
SMCLOG = os.path.join(HERE, "smclog.py")
BID = "ab" * 16


def read(path, mode="r"):
    with open(path, mode) as f:
        return f.read()


def header(t=0, build_id=BID, lost=0):
    bid = bytes.fromhex(build_id)
    return sl.frame(struct.pack("<BBIIIIB", sl.KIND_HEADER, 1, 1000000, t, lost, 8192, len(bid)) + bid)


def log(seq, t, sid, cause=0, args=b"", kind=sl.KIND_LOG):
    return sl.frame(struct.pack("<BHIHH", kind, seq, t, sid, cause) + args)


SITES = {"sites": {"0": {"level": "INFO", "module": "test", "file": "f.c", "line": 1,
                          "format": "v=%d", "types": ["int"]},
                    "5": {"level": "WARN", "module": "input", "file": "f.c", "line": 2,
                          "format": "plain", "types": []}},
         "build_id": BID, "format": 1}


class Basics(unittest.TestCase):
    def test_crc(self):
        self.assertEqual(sl.crc16(b"123456789"), 0x29B1)

    def test_cobs_vectors(self):
        self.assertEqual(sl.cobs_encode(b""), b"\x01")
        self.assertEqual(sl.cobs_encode(b"\x00"), b"\x01\x01")
        self.assertEqual(sl.cobs_encode(b"\x11\x22\x00\x33"), b"\x03\x11\x22\x02\x33")
        run = bytes(range(1, 255))
        self.assertEqual(sl.cobs_encode(run), b"\xff" + run + b"\x01")

    def test_cobs_round_trip(self):
        for data in (b"", b"\0", b"\0\0", b"a\0b", bytes(range(1, 255)), bytes(range(256)) * 3,
                     b"\x01" * 600, bytes(range(1, 255)) + b"\0"):
            enc = sl.cobs_encode(data)
            self.assertNotIn(0, enc)
            self.assertEqual(sl.cobs_decode(enc), data)

    def test_cobs_bad(self):
        self.assertIsNone(sl.cobs_decode(b"\x05ab"))

    def test_render(self):
        r = sl.render
        self.assertEqual(r("a %d b", [(-5, 32)]), "a -5 b")
        self.assertEqual(r("%u %u", [(-1, 32), (-1, 64)]), "4294967295 18446744073709551615")
        self.assertEqual(r("%x %X %08x %o", [(48879, 32), (48879, 32), (2748, 32), (8, 32)]),
                         "beef BEEF 00000abc 10")
        self.assertEqual(r("%c%c", [(65, 32), (0x142, 32)]), "AB")
        self.assertEqual(r("%p", [(0x1234, 64)]), "0x1234")
        self.assertEqual(r("[%5s] [%-4s]", [("ab", 0), ("cd", 0)]), "[   ab] [cd  ]")
        self.assertEqual(r("%.2f %g", [(1.5, 32), (-0.25, 32)]), "1.50 -0.25")
        self.assertEqual(r("100%% %lu %zu %hhu", [(1, 32), (2, 64), (3, 32)]), "100% 1 2 3")
        self.assertEqual(r("%d %d", [(1, 32)]), "1 <?>")
        self.assertEqual(r("%d %d", [(1, 32), None]), "1 <?>")
        self.assertEqual(r("%d", [("x", 0)]), "<%d? 'x'>")
        self.assertEqual(r("no args", []), "no args")

    def test_decode_args(self):
        b = bytes([0x01, 0x80, 0x01]) + struct.pack("<f", 2.5) + b"\x02hi" + b"\x83abc"
        self.assertEqual(sl.decode_args(["int", "i64", "f32", "str", "str"], b),
                         [(-1, 32), (64, 64), (2.5, 32), ("hi", 0), ("abc...", 0)])
        self.assertEqual(sl.decode_args(["int", "int"], b"\x02"), [(1, 32), None])
        self.assertEqual(sl.decode_args(["int"], b"\x80"), [None])


class Decoding(unittest.TestCase):
    def test_records_and_header(self):
        d = sl.Decoder(SITES)
        out = d.feed(header(5) + log(0, 10, 0, 3, b"\x01") + log(1, 20, 5), t_host_ns=99)
        self.assertEqual([r["kind"] for r in out], ["header", "log", "log"])
        self.assertEqual(out[0]["build_id"], BID)
        self.assertEqual((out[1]["msg"], out[1]["cause"], out[1]["level"], out[1]["t_us"]), ("v=-1", 3, "INFO", 10))
        self.assertEqual((out[2]["msg"], out[2]["module"]), ("plain", "input"))
        self.assertEqual(out[2]["t_host_ns"], 99)

    def test_split_feed(self):
        d = sl.Decoder(SITES)
        stream = header() + log(0, 10, 0, 0, b"\x02")
        out = []
        for i in range(len(stream)):
            out += d.feed(stream[i:i + 1])
        self.assertEqual([r["kind"] for r in out], ["header", "log"])
        self.assertEqual(out[1]["msg"], "v=1")

    def test_time_wraps(self):
        d = sl.Decoder(SITES)
        out = d.feed(header(0xFFFFFF00) + log(0, 0xFFFFFFF0, 5) + log(1, 0x100, 5))
        self.assertEqual(out[2]["t_us"], (1 << 32) + 0x100)

    def test_seq_gap_and_lost(self):
        d = sl.Decoder(SITES)
        lost = sl.frame(struct.pack("<BHII", sl.KIND_LOST, 9, 30, 4))
        out = d.feed(header() + log(5, 10, 5) + log(8, 20, 5) + lost)
        self.assertEqual([r["kind"] for r in out], ["header", "log", "gap", "log", "lost"])
        self.assertEqual(out[2]["missing"], 2)
        self.assertEqual(out[4]["count"], 4)

    def test_seq_wraps_without_gap(self):
        d = sl.Decoder(SITES)
        out = d.feed(header() + log(0xFFFF, 10, 5) + log(0, 20, 5))
        self.assertNotIn("gap", [r["kind"] for r in out])

    def test_build_id_mismatch_is_an_error(self):
        d = sl.Decoder(SITES)
        with self.assertRaises(sl.BuildIdMismatch):
            d.feed(header(build_id="cd" * 16))

    def test_bad_frame_then_good(self):
        d = sl.Decoder(SITES)
        out = d.feed(b"\x07garbage\x00" + header() + log(0, 1, 5))
        self.assertEqual([r["kind"] for r in out], ["bad_frame", "header", "log"])

    def test_no_header_warns_once(self):
        d = sl.Decoder(SITES)
        out = d.feed(log(0, 1, 5) + log(1, 2, 5))
        self.assertEqual([r["kind"] for r in out], ["warning", "log", "log"])

    def test_past_records(self):
        d = sl.Decoder(SITES)
        past = log(40, 0xFFFFFF00, 0, 0, b"\x04", kind=sl.KIND_LOG | sl.KIND_PAST)
        out = d.feed(header(100) + past + log(0, 200, 5) + log(1, 300, 5))
        self.assertEqual([r["kind"] for r in out], ["header", "log", "log", "log"])  # no gap
        self.assertEqual((out[1]["past"], out[1]["msg"], out[1]["seq"], out[1]["t_us"]), (True, "v=2", 40, 0xFFFFFF00))
        self.assertEqual(out[2]["t_us"], 200)  # not unwrapped against the previous run's clock
        self.assertNotIn("past", out[2])
        self.assertTrue(sl.as_text(out[1]).startswith("prev4294.967 INFO  test     v=2"))
        trunc = log(3, 7, 0, 0, b"", kind=sl.KIND_LOG_TRUNC | sl.KIND_PAST)
        r = d.feed(trunc)[0]
        self.assertEqual((r["past"], r["truncated"]), (True, True))

    def test_unknown_site_and_truncated(self):
        d = sl.Decoder(SITES)
        out = d.feed(header() + log(0, 1, 77, 0, b"\x01") + log(1, 2, 0, 0, b"", sl.KIND_LOG_TRUNC))
        self.assertEqual(out[1]["error"], "unknown call site")
        self.assertEqual((out[2]["msg"], out[2]["truncated"]), ("v=<?>", True))


@unittest.skipUnless(os.path.exists(TEST_BIN), "build/host/test_smclog missing (make -C firmware test)")
class WithTestBinary(unittest.TestCase):
    FILL = 60

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.stream_path = os.path.join(cls.tmp.name, "stream.bin")
        subprocess.run([TEST_BIN, "--emit", cls.stream_path], check=True)
        cls.stream = read(cls.stream_path, "rb")
        cls.dictionary = sl.make_dictionary(TEST_BIN)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_dictionary(self):
        d = self.dictionary
        self.assertIn(len(d["build_id"]), (32, 40))  # LC_UUID on macOS, the GNU build ID on Linux
        formats = {s["format"]: s for s in d["sites"].values()}
        self.assertEqual(formats["hello"]["module"], "test")
        self.assertEqual(formats["ints %d %u %x %X %o %c"]["types"], ["int"] * 6)
        self.assertEqual(formats["float %.2f %g"]["types"], ["f32", "f32"])
        self.assertEqual(formats["i64 %lld %llu %llx"]["types"], ["i64"] * 3)
        self.assertEqual(formats["hello"]["file"], "tests/test_smclog.c")

    def test_emitted_stream(self):
        out = sl.Decoder(self.dictionary).feed(self.stream)
        kinds = [r["kind"] for r in out]
        self.assertEqual(kinds[0], "header")
        self.assertEqual(out[0]["build_id"], self.dictionary["build_id"])
        self.assertEqual(out[0]["ring_size"], 512)
        for bad in ("gap", "bad_frame", "unknown", "warning"):
            self.assertNotIn(bad, kinds)
        logs = [(r["level"], r["module"], r["msg"], r["cause"]) for r in out if r["kind"] == "log"]
        self.assertEqual(logs[:11], [
            ("INFO", "smclog", "log start: format 1, ring 512 bytes, records up to 96 bytes", 0),
            ("INFO", "test", "hello", 0),
            ("WARN", "test", "ints -5 4000000000 beef BEEF 10 A", 0),
            ("ERROR", "test", "i64 -9223372036854775808 18446744073709551615 123456789a", 0),
            ("DEBUG", "test", "float 1.50 -0.25", 0),
            ("TRACE", "test", "str 'abc' '' [   ab] [cd  ]", 0),
            ("INFO", "test", "percent 100% of 3, hex 00000abc", 0),
            ("INFO", "test", "ptr 0x1234", 0),
            ("INFO", "test", "eight 1 2 3 4 5 6 7 8", 0),
            ("INFO", "input", "fader 3 = 100", 1),
            ("INFO", "engine", "explicit cause", 7),
        ])
        fills = [m for _, _, m, _ in logs[11:-1]]
        self.assertEqual(fills, ["fill %d" % i for i in range(len(fills))])
        lost = [r for r in out if r["kind"] == "lost"]
        self.assertEqual(len(lost), 1)
        self.assertEqual(lost[0]["count"] + len(fills), self.FILL)
        self.assertEqual(logs[-1][2], "after loss")
        times = [r["t_us"] for r in out if "t_us" in r]
        self.assertEqual(times, sorted(times))
        self.assertEqual(len(set(times)), len(times))

    def test_emitted_past_stream(self):
        path = os.path.join(self.tmp.name, "past.bin")
        subprocess.run([TEST_BIN, "--emit-past", path], check=True)
        out = sl.Decoder(self.dictionary).feed(read(path, "rb"))
        kinds = [r["kind"] for r in out]
        self.assertEqual(kinds, ["header"] + ["log"] * 5)
        logs = [(r.get("past", False), r["level"], r["msg"]) for r in out[1:]]
        self.assertEqual(logs, [
            (False, "INFO", "log start: format 1, ring 512 bytes, records up to 96 bytes"),
            (True, "INFO", "before reset 1"), (True, "WARN", "before reset 2"), (True, "INFO", "before reset 3"),
            (False, "INFO", "after reset")])
        self.assertEqual([r["t_us"] for r in out[2:5]], [900000, 901000, 902000])  # the previous run's clock
        self.assertEqual([r["seq"] for r in out[2:5]], [0, 1, 2])

    def test_tampered_dictionary(self):
        d = dict(self.dictionary, build_id="00" * 16)
        with self.assertRaises(sl.BuildIdMismatch):
            sl.Decoder(d).feed(self.stream)

    def test_cli_dict_and_decode(self):
        dpath = os.path.join(self.tmp.name, "dict.json")
        subprocess.run([sys.executable, "-I", SMCLOG, "dict", TEST_BIN, "-o", dpath], check=True,
                       capture_output=True)
        text = subprocess.run([sys.executable, "-I", SMCLOG, "decode", "--dict", dpath, self.stream_path,
                               "--text"], check=True, capture_output=True, text=True).stdout
        self.assertIn(" INFO  test     hello", text)
        self.assertIn("LOST", text)
        jl = subprocess.run([sys.executable, "-I", SMCLOG, "decode", "--binary", TEST_BIN, self.stream_path],
                            check=True, capture_output=True, text=True).stdout
        self.assertEqual(len(jl.splitlines()), len(sl.Decoder(self.dictionary).feed(self.stream)))

    def test_capture_through_pty(self):
        master, slave = os.openpty()
        session = os.path.join(self.tmp.name, "session")
        p = subprocess.Popen([sys.executable, "-I", SMCLOG, "capture", "--port", os.ttyname(slave),
                              "--binary", TEST_BIN, "--session", session, "--seconds", "2", "--quiet",
                              "--no-subscribe"],  # a stream that runs by itself
                             stdin=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            self.assertIn("smclog capture:", p.stderr.readline())  # the port is open
            half = len(self.stream) // 2
            os.write(master, self.stream[:half])
            p.stdin.write("first marker\n")
            p.stdin.flush()
            time.sleep(0.2)
            os.write(master, self.stream[half:])
            p.stdin.write("\n")
            p.stdin.close()
            self.assertEqual(p.wait(timeout=10), 0)
        finally:
            os.close(master)
            os.close(slave)
            p.stderr.close()
        self.assertEqual(read(os.path.join(session, "device.bin"), "rb"), self.stream)
        recs = [json.loads(x) for x in read(os.path.join(session, "device.jsonl")).splitlines()]
        want = sl.Decoder(self.dictionary).feed(self.stream)
        self.assertEqual([r["kind"] for r in recs], [r["kind"] for r in want])
        self.assertTrue(all("t_host_ns" in r for r in recs))
        markers = [json.loads(x) for x in read(os.path.join(session, "markers.jsonl")).splitlines()]
        self.assertEqual([m["text"] for m in markers], ["first marker", ""])
        meta = json.loads(read(os.path.join(session, "meta.json")))
        self.assertEqual(meta["build_id"], self.dictionary["build_id"])


def reply(seq, cmd, status=0, payload=b""):
    return sl.frame(bytes([sl.KIND_REPLY, seq, cmd, status]) + payload)


class LinkUnitTests(unittest.TestCase):
    def test_command_frame(self):
        f = sl.command_frame(0x105, 0x22, b"\x03\x04")
        self.assertEqual(f[0], 0)  # ends a partial frame on the device
        self.assertEqual(f[-1], 0)
        rec = sl.cobs_decode(f[1:-1])
        self.assertEqual(rec[:-2], bytes([sl.KIND_CMD, 0x05, 0x22, 3, 4]))
        self.assertEqual(sl.crc16(rec[:-2]), struct.unpack("<H", rec[-2:])[0])

    def test_replies(self):
        bid = bytes(range(16))
        info = struct.pack("<BBBIIIB", 1, 1, 1, 0b1011, 8192, 1000000, len(bid)) + bid
        stream = (reply(1, 0x20) + reply(2, 0x01, 0, struct.pack("<IQ", 5000, 123)) + reply(3, 0x02, 0, info)
                  + reply(4, 0x23, 0, bytes([3, 3, 4, 2])) + reply(5, 0x30, 0, bytes([4, 1, 2]))
                  + reply(6, 0x24, 4) + reply(7, 0x7E, 1) + reply(8, 0x31, 0, bytes([2, 3])))
        out = sl.Decoder({"sites": {}}).feed(stream)
        self.assertEqual([r["kind"] for r in out], ["reply"] * 8)  # no "before a header" warning
        self.assertEqual((out[0]["cmd"], out[0]["status"]), ("subscribe", "ok"))
        self.assertEqual((out[1]["t_us"], out[1]["t_host_send_ns"]), (5000, 123))
        self.assertEqual(out[2]["variant"], "hwtest")
        self.assertEqual(out[2]["caps"], ["log", "raw_input", "metrics"])
        self.assertEqual(out[2]["build_id"], bid.hex())
        self.assertEqual(out[3]["levels"], [3, 4, 2])
        self.assertEqual((out[4]["inject"], out[4]["accepted"], out[4]["refused"]), ("full", 1, 2))
        self.assertEqual((out[5]["cmd"], out[5]["status"]), ("raw", "unavailable"))
        self.assertEqual((out[6]["cmd"], out[6]["status"]), ("0x7e", "unknown command"))
        self.assertEqual((out[7]["cmd"], out[7]["setup"], out[7]["bad_pair"]), ("setup", "bad setting", 3))
        self.assertIn("REPLY subscribe seq 1: ok", sl.as_text(out[0]))

    def test_reset(self):
        out = sl.Decoder({"sites": {}}).feed(reply(1, 0x27, 0, b"\x01") + reply(2, 0x27, 0, b"\x00") + reply(3, 0x27, 3))
        self.assertEqual([(r["cmd"], r["status"], r.get("reset")) for r in out],
                         [("reset", "ok", "disabled"), ("reset", "ok", "ok"), ("reset", "bad argument", None)])
        payload = [sl.command_payload(types.SimpleNamespace(command="reset", args=[m]), {"sites": {}})
                   for m in ("soft", "fault", "watchdog")]
        self.assertEqual(payload, [b"\x00", b"\x01", b"\x02"])
        for bad in ([], ["hard"], ["soft", "fault"]):
            with self.assertRaises(sl.SmclogError):
                sl.command_payload(types.SimpleNamespace(command="reset", args=bad), {"sites": {}})
        self.assertEqual(sl.CAP_NAMES[6], "reset")

    def test_stock(self):
        out = sl.Decoder({"sites": {}}).feed(reply(1, 0x29, 0, b"\x00\x00\x10\x04\x00\x00")
                                             + reply(2, 0x29, 0, b"\x04\x00\x10\x04\x00\x03") + reply(3, 0x29, 3))
        self.assertEqual([(r["cmd"], r["status"], r.get("stock"), r.get("pointer"), r.get("tries")) for r in out],
                         [("stock", "ok", "ok", 0x41000, 0), ("stock", "ok", "flash", 0x41000, 3),
                          ("stock", "bad argument", None, None, None)])
        payload = [sl.command_payload(types.SimpleNamespace(command="stock", args=[m]), {"sites": {}})
                   for m in ("check", "back")]
        self.assertEqual(payload, [b"\x00", b"\x01BACK"])
        with self.assertRaises(sl.SmclogError):
            sl.command_payload(types.SimpleNamespace(command="stock", args=["now"]), {"sites": {}})
        self.assertEqual((sl.CAP_NAMES[10], sl.CMDS["stock"]), ("stock", 0x29))

    def test_raw_payload(self):
        payload = [sl.command_payload(types.SimpleNamespace(command="raw", args=[m]), {"sites": {}})
                   for m in ("off", "matrix", "faders", "on")]
        self.assertEqual(payload, [b"\x00", b"\x01", b"\x02", b"\x03"])  # link.h's LINK_RAW_* bits
        probe = sl.command_payload(types.SimpleNamespace(command="raw", args=["probe7"]), {"sites": {}})
        self.assertEqual(probe, b"\x71")  # the matrix bit and encoder 7 in bits 4-7
        for bad in ([], ["1"], ["on", "off"], ["probe9"], ["probe0"]):
            with self.assertRaises(sl.SmclogError):
                sl.command_payload(types.SimpleNamespace(command="raw", args=bad), {"sites": {}})

    def test_inject_payload(self):
        self.assertEqual(sl.inject_payload(["on"]), b"\x01")
        self.assertEqual(sl.inject_payload(["off"]), b"\x00")
        p = sl.inject_payload(["button:3:press", "encoder:1:-2", "fader:7:16383", "midi:ble:b0,07,7f"])
        self.assertEqual(p, bytes([2, 1, 3, 1, 0, 0, 2, 1, 0xFE, 0xFF, 0, 3, 7, 0xFF, 0x3F, 0, 4, 1, 0xB0, 7, 0x7F]))
        for bad in (["button:3"], ["knob:1:2"], ["midi:usb:90,40"], ["encoder:x:1"], ["button:3:hold"]):
            with self.assertRaises(sl.SmclogError):
                sl.inject_payload(bad)

    def test_setup_payload(self):
        self.assertEqual(sl.setup_payload([]), b"")
        self.assertEqual(sl.setup_payload(["config=test", "note_off_p2=8n"]), b"config test\nnote_off_p2 8n")
        for bad in (["config"], ["=test"], ["config="], ["a=b c"], ["gesture_ms=%s" % ("1" * 200)]):
            with self.assertRaises(sl.SmclogError):
                sl.setup_payload(bad)

    def test_levels_and_modules(self):
        d = {"sites": {"0": {"module": "input", "module_index": 3}}}
        self.assertEqual(sl.module_index("input", d), 3)
        self.assertEqual(sl.module_index("all", d), 0xFF)
        self.assertEqual(sl.module_index("7", d), 7)
        with self.assertRaises(sl.SmclogError):
            sl.module_index("engine", d)
        self.assertEqual([sl.level_value(x) for x in ("off", "ERROR", "debug", "5")], [0, 1, 4, 5])
        with self.assertRaises(sl.SmclogError):
            sl.level_value("loud")

    def test_no_reply(self):
        master, slave = os.openpty()
        try:
            p = subprocess.run([sys.executable, "-I", SMCLOG, "cmd", "--port", os.ttyname(slave), "--timeout", "0.1",
                                "ping"], capture_output=True, text=True, timeout=10)
        finally:
            os.close(master)
            os.close(slave)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("no reply to ping after 3 tries", p.stderr)


@unittest.skipUnless(os.path.exists(LINK_BIN), "build/host/test_link not built (make -C firmware test)")
class LinkDeviceTests(unittest.TestCase):
    """The tool against the firmware's link code (test_link --serve) through a pseudo-terminal."""

    @classmethod
    def setUpClass(cls):
        cls.dictionary = sl.make_dictionary(LINK_BIN)

    def setUp(self):
        import tty
        self.master, self.slave = os.openpty()
        tty.setraw(self.slave)  # no echo of the device's bytes back to it
        self.dev = subprocess.Popen([LINK_BIN, "--serve", "30"], stdin=self.master, stdout=self.master)
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.dev.kill()
        self.dev.wait()
        os.close(self.master)
        os.close(self.slave)
        self.tmp.cleanup()

    def cmd(self, *args, check=True):
        p = subprocess.run([sys.executable, "-I", SMCLOG, "cmd", "--port", os.ttyname(self.slave), "--binary",
                            LINK_BIN] + list(args), capture_output=True, text=True, timeout=20)
        if check:
            self.assertEqual(p.returncode, 0, p.stderr)
        return p

    def test_info_and_ping(self):
        r = json.loads(self.cmd("--json", "info").stdout)
        self.assertEqual((r["status"], r["link_version"], r["variant"]), ("ok", 1, "hwtest"))
        self.assertEqual(r["caps"], ["log", "raw_input", "inject", "metrics", "setup"])
        self.assertEqual(r["build_id"], self.dictionary["build_id"])
        self.assertEqual(r["fw_version"], "1.23.4")  # test_link --serve
        self.assertIn("firmware 1.23.4, link 1", self.cmd("info").stdout)
        r = json.loads(self.cmd("--json", "ping", "hello").stdout)
        self.assertEqual((r["status"], r["echo"]), ("ok", b"hello".hex()))
        self.assertIn("round trip", self.cmd("ping").stdout)

    def test_levels(self):
        self.assertEqual(self.cmd("level", "test", "debug").stdout.strip(), "level: ok")
        self.assertIn("test=DEBUG", self.cmd("levels").stdout)
        self.cmd("level", "all", "warn")
        self.assertIn("link=WARN", self.cmd("levels").stdout)
        p = self.cmd("level", "nosuch", "info", check=False)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("not in the dictionary", p.stderr)

    def test_variant_commands(self):
        self.assertEqual(self.cmd("raw", "on").stdout.strip(), "raw: ok")
        self.assertEqual(self.cmd("metrics").stdout.strip(), "metrics: ok")
        out = self.cmd("inject", "button:3:press", "encoder:1:-2").stdout.strip()
        self.assertEqual(out, "inject: ok, 2 accepted, 0 refused")
        self.assertEqual(self.cmd("setup", "config=test", "note_off=8n").stdout.strip(), "setup: ok")
        # The persistent PTY stand-in does not reset its retry cache on CLI exit as
        # real CDC does on DTR close. Replace its prior SETUP cache entry so two
        # independently seeded CLI sequences cannot alias the preceding request.
        self.cmd("ping")
        p = self.cmd("setup", "a=1", "bad=2", check=False)  # the stand-in refuses "bad" as pair 2
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(p.stdout.strip(), "setup: bad setting (setting 2)")

    def test_variant_commands_with_colliding_cli_sequences(self):
        from unittest.mock import patch
        run = subprocess.run
        for seed in (0, 42, 255):
            def seeded_run(argv, **kwargs):
                if argv[:3] == [sys.executable, "-I", SMCLOG]:
                    code = ("import os,runpy,sys;os.urandom=lambda n:bytes([" + str(seed) +
                            "])*n;sys.argv=sys.argv[1:];runpy.run_path(sys.argv[0],run_name='__main__')")
                    argv = [sys.executable, "-I", "-c", code] + argv[2:]
                return run(argv, **kwargs)
            with self.subTest(seed=seed), patch.object(subprocess, "run", seeded_run):
                self.test_variant_commands()

    def test_capture_subscribes(self):
        session = os.path.join(self.tmp.name, "s")
        p = subprocess.run([sys.executable, "-I", SMCLOG, "capture", "--port", os.ttyname(self.slave), "--binary",
                            LINK_BIN, "--session", session, "--seconds", "2.5", "--quiet", "--no-markers",
                            "--sync", "0.5"], capture_output=True, text=True, timeout=20)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertNotIn("no reply", p.stderr)
        recs = [json.loads(x) for x in read(os.path.join(session, "device.jsonl")).splitlines()]
        kinds = [r["kind"] for r in recs]
        self.assertEqual(kinds[0], "reply")  # the subscription's
        self.assertEqual((recs[0]["cmd"], recs[0]["status"]), ("subscribe", "ok"))
        self.assertEqual(kinds[1], "header")
        self.assertEqual(recs[1]["build_id"], self.dictionary["build_id"])
        logs = [r for r in recs if r["kind"] == "log"]
        self.assertTrue(any(r["msg"] == "log stream on" for r in logs))
        ticks = [r for r in logs if r["msg"].startswith("tick ")]
        self.assertGreaterEqual(len(ticks), 10)  # one every 100 ms; the ring kept the early ones
        self.assertNotIn("bad_frame", kinds)
        self.assertNotIn("gap", kinds)
        pings = [r for r in recs if r["kind"] == "reply" and r["cmd"] == "ping"]
        self.assertGreaterEqual(len(pings), 3)
        for r in pings:  # clock sync: the device's time between the host's send and receive
            self.assertLessEqual(r["t_host_send_ns"], r["t_host_ns"])
            self.assertIn("t_us", r)
        meta = json.loads(read(os.path.join(session, "meta.json")))
        self.assertTrue(meta["subscribe"])
        # after the capture (which unsubscribes on exit) the device still answers
        r = json.loads(self.cmd("--json", "ping").stdout)
        self.assertEqual(r["status"], "ok")


def ping_reply(send_ns, t_us, recv_ns):
    return {"kind": "reply", "cmd": "ping", "status": "ok", "t_us": t_us, "t_host_send_ns": send_ns,
            "t_host_ns": recv_ns}


def clock_samples(n, drift_ppm, period_s=1.0, start_us=5000, rtt_us=(300, 2000), h0=10 ** 12, seed=1):
    """PING samples of a device whose clock runs drift_ppm fast; it reads its clock somewhere inside
    each round trip."""
    rnd = random.Random(seed)
    out = []
    for i in range(n):
        send = h0 + int(i * period_s * 1e9)
        rtt = rnd.uniform(*rtt_us) * 1e3
        read = send + rtt * rnd.uniform(0.2, 0.8)
        out.append((send, int(start_us + (read - h0) / 1e3 * (1 + drift_ppm / 1e6)), int(send + rtt)))
    return out


class Clock(unittest.TestCase):
    """The device's clock against the host's, from PING samples (LOG-20)."""

    def test_drift(self):
        fits = sl.clock_fit(clock_samples(60, 120.0))
        self.assertEqual(len(fits), 1)
        f = fits[0]
        self.assertEqual((f["samples"], f["used"]), (60, 60))
        self.assertAlmostEqual(f["drift_ppm"], 120.0, delta=15)
        self.assertLess(f["drift_se_ppm"], 10)
        self.assertAlmostEqual(f["span_s"], 59.0)
        self.assertGreaterEqual(f["rtt_us"]["min"], 300)
        self.assertLessEqual(f["rtt_us"]["max"], 2000)
        # the line maps the device's times back to the host's within the round trips
        for send, t_us, recv in clock_samples(60, 120.0)[::7]:
            self.assertLess(abs(sl.device_to_host_ns(f, t_us) - (send + recv) / 2), 1.5e6)

    def test_a_burst_counts_once(self):
        """Quick PINGs close together must not decide the slope (a burst at the start, then one a
        second with slower round trips)."""
        burst = [(10 ** 12 + i * 100000, 5000 + i * 100 + (7 if i % 2 else 0), 10 ** 12 + i * 100000 + 50000)
                 for i in range(20)]
        rest = clock_samples(15, 0.0, h0=10 ** 12 + 10 ** 9, start_us=5000 + 10 ** 6)
        f = sl.clock_fit(burst + rest)[0]
        self.assertEqual((f["samples"], f["used"]), (35, 16))
        self.assertAlmostEqual(f["drift_ppm"], 0.0, delta=60)

    def test_runs_and_a_single_sample(self):
        first = clock_samples(10, 50.0, start_us=3000000)
        second = clock_samples(1, 0.0, h0=10 ** 12 + 12 * 10 ** 9)  # after a reset: the clock starts again
        fits = sl.clock_fit(first + second)
        self.assertEqual([f["samples"] for f in fits], [10, 1])
        self.assertIsNone(fits[1]["drift_ppm"])
        self.assertIsNone(fits[1]["drift_se_ppm"])
        self.assertIn("drift not known (one sample)", sl.clock_text(fits))
        self.assertEqual(sl.clock_fit([]), [])

    def test_a_reset_up_longer_than_the_run_before(self):
        """After a reset the clock may read more than before it, if the run before was short: the
        device's time lagging far behind the host's marks the reset too."""
        first = clock_samples(3, 0.0, start_us=700000)
        second = clock_samples(10, 30.0, h0=10 ** 12 + 11 * 10 ** 9, start_us=1200000)
        fits = sl.clock_fit(first + second)
        self.assertEqual([f["samples"] for f in fits], [3, 10])
        self.assertAlmostEqual(fits[1]["drift_ppm"], 30.0, delta=60)
        self.assertIn("no PING", sl.clock_text([]))

    def test_samples_and_the_tool(self):
        recs = [ping_reply(*s) for s in clock_samples(30, -40.0)]
        recs.insert(3, {"kind": "reply", "cmd": "ping", "status": "ok", "t_us": 1, "t_host_ns": 5})  # no send time
        recs.insert(5, {"kind": "log", "msg": "x", "t_us": 9})
        self.assertEqual(len(sl.clock_samples(recs)), 30)
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "device.jsonl")
            with open(path, "w") as f:
                f.writelines(json.dumps(r) + "\n" for r in recs)
            p = subprocess.run([sys.executable, "-I", SMCLOG, "clock", tmp], capture_output=True, text=True, timeout=20)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertIn("run 1: 30 PINGs over 29.0 s, 30 used", p.stdout)
            p = subprocess.run([sys.executable, "-I", SMCLOG, "clock", "--json", path], capture_output=True, text=True,
                               timeout=20)
            self.assertAlmostEqual(json.loads(p.stdout)[0]["drift_ppm"], -40.0, delta=20)


def metric(t, msg, args):
    return {"kind": "log", "module": "metrics", "t_us": t, "msg": msg, "args": args}


def beat(t, lat_usb, stack_unused=1200, lost=0, load=(200, 200000), adc_timeouts=0):
    """One heartbeat's records; lat_usb: (n, max, buckets); load: (busy, samples)."""
    n, mx, buckets = lat_usb
    return [metric(t, "heartbeat 1: 10000 ms; CPU of our code, per mille: scan 30, control 2, USB 1", [1, 10000, 30, 2, 1]),
            metric(t, "CPU load: ...", [load[0] * 1000 // load[1], load[0], load[1]]),
            metric(t, "fader ADC: ...", [40000, 2000, 7, 1, adc_timeouts, 5]),
            metric(t, "scan steps: ...", [200000, 3, 4100, 9000, 7]),
            metric(t, "scan passes: ...", [12500, 790, 800, 950]),
            metric(t, "clock: ...", [0, 120, 980]),
            metric(t, "latency input to core: ...", [0, 0, 0, 0, 0, 0]),
            metric(t, "latency input to USB: ...", [n, 100, 300, mx, 0, 0]),
            metric(t, "latency input to USB, events by bucket: ...", buckets),
            metric(t, "traffic: ...", [n, n, 0, 0, 2, 3, 0, 0]),
            metric(t, "log: ...", [40, lost, 900, 8192]),
            metric(t, "heap: ...", [70000, 65000]),
            metric(t, "stack smc_usb: 1200 of 2048 bytes free below the deepest point sampled",
                   ["smc_usb", stack_unused, 2048]),
            metric(t, "stack smc_ctl: 3072 bytes, not measured (bounds not found)", ["smc_ctl", 3072])]


class CaptureWindowMetrics(unittest.TestCase):
    """Buffered old timestamps can appear ahead of a new raw-u32 capture header."""

    def records(self, wrap=False, complete=True):
        mod, host = 1 << 32, 10**12
        raw_start = mod - 500000 if wrap else 3451000000
        aligned = 2 * mod - 500000 if wrap else raw_start
        header = {'kind': 'header', 't_us': raw_start, 't_host_ns': host, 'lost_total': 99}
        stale = {'kind': 'reply', 'cmd': 'ping', 'status': 'ok', 't_us': 123,
                 't_host_send_ns': host - 1000000, 't_host_ns': host + 500000}
        sync = {'kind': 'reply', 'cmd': 'ping', 'status': 'ok', 't_us': aligned + 1000000,
                't_host_send_ns': host + 1000000000, 't_host_ns': host + 1001000000}
        def timed(device, after_ns, lost=0):
            return [dict(r, t_host_ns=host + after_ns) for r in beat(device, (0, 0, [0] * 8), lost=lost)]
        records = [header, stale] + timed(aligned + 130000000, 1000000, lost=9) + [sync]
        records += timed(aligned + 5000000, 5010000000)
        if complete:
            records += timed(aligned + 15000000, 15010000000)
        return records, aligned

    def test_buffered_future_looking_records_and_partial_window_are_excluded(self):
        records, start = self.records()
        before = json.dumps(records, sort_keys=True)
        all_beats = sl.summarize_metrics(sl.read_metrics(records))
        current = sl.summarize_capture_metrics(records)
        self.assertEqual(all_beats['heartbeats'], 3)
        self.assertEqual(all_beats['log']['lost'], 9)
        self.assertEqual((current['heartbeats'], current['log']['lost']), (1, 0))
        self.assertEqual(current['capture_window'], {'included': 1, 'excluded': 2,
                         'start_device_us': start, 'start_host_ns': 10**12, 'initial_lost_total': 99})
        self.assertEqual(before, json.dumps(records, sort_keys=True))

    def test_capture_epoch_is_aligned_with_fresh_ping_across_wrap(self):
        records, aligned = self.records(wrap=True)
        current = sl.summarize_capture_metrics(records)
        self.assertEqual(current['capture_window']['start_device_us'], aligned)
        self.assertEqual(current['heartbeats'], 1)
        self.assertEqual(current['failed'], [])

    def test_clock_sync_can_arrive_after_multiple_wraps(self):
        records, aligned = self.records()
        records = [r for r in records if not (r.get('kind') == 'reply' and r.get('t_host_send_ns', 0) >= 10**12)]
        elapsed = 2 * (1 << 32) + 1000000
        records.append({'kind': 'reply', 'cmd': 'ping', 'status': 'ok', 't_us': aligned + elapsed,
                        't_host_send_ns': 10**12 + elapsed * 1000,
                        't_host_ns': 10**12 + elapsed * 1000 + 1000000})
        current = sl.summarize_capture_metrics(records)
        self.assertEqual(current['capture_window']['start_device_us'], aligned)
        self.assertEqual(current['heartbeats'], 1)

    def test_missing_header_or_fresh_clock_sync_is_not_silently_accepted(self):
        records, _ = self.records()
        for bad in (records[1:], records + [dict(records[0])]):
            with self.assertRaisesRegex(sl.SmclogError, 'one stream header'):
                sl.summarize_capture_metrics(bad)
        without_sync = [r for r in records if not (r.get('kind') == 'reply' and r.get('t_host_send_ns', 0) >= 10**12)]
        with self.assertRaisesRegex(sl.SmclogError, 'clock-sync PINGs'):
            sl.summarize_capture_metrics(without_sync)

    def test_no_complete_window_fails_the_requested_check(self):
        records, _ = self.records(complete=False)
        current = sl.summarize_capture_metrics(records)
        self.assertEqual(current['heartbeats'], 0)
        self.assertIn('no complete heartbeat window', current['failed'][0])
        self.assertIn('FAIL no complete heartbeat window', sl.metrics_text(current))

    def test_cli_flag_filters_and_preserves_the_default(self):
        for complete in (True, False):
            records, _ = self.records(complete=complete)
            with tempfile.TemporaryDirectory() as tmp:
                path = os.path.join(tmp, 'device.jsonl')
                with open(path, 'w') as f:
                    f.writelines(json.dumps(r) + '\n' for r in records)
                p = subprocess.run([sys.executable, '-I', SMCLOG, 'metrics', path,
                                    '--capture-window', '--json', '--check'],
                                   capture_output=True, text=True, timeout=20)
                self.assertEqual(p.returncode, 0 if complete else 1, p.stderr)
                self.assertEqual(json.loads(p.stdout)['heartbeats'], 1 if complete else 0)
                p = subprocess.run([sys.executable, '-I', SMCLOG, 'metrics', path, '--json'],
                                   capture_output=True, text=True, timeout=20)
                self.assertEqual(p.returncode, 0, p.stderr)
                self.assertEqual(json.loads(p.stdout)['heartbeats'], 3 if complete else 2)
                self.assertNotIn('capture_window', json.loads(p.stdout))


class Metrics(unittest.TestCase):
    """The heartbeat's records (LOG-06) read back and summed up."""

    def test_percentile(self):  # the same cases as firmware/tests/test_metrics.c
        self.assertEqual(sl.lat_percentile([0] * 8, 0, 500), 0)
        self.assertEqual(sl.lat_percentile([1, 0, 0, 0, 0, 0, 0, 0], 120, 500), 120)
        b = [0, 98, 0, 0, 0, 0, 2, 0]
        self.assertEqual(sl.lat_percentile(b, 7000, 500), 500)
        self.assertEqual(sl.lat_percentile(b, 7000, 980), 500)
        self.assertEqual(sl.lat_percentile(b, 7000, 990), 7000)
        self.assertEqual(sl.lat_percentile([0, 98, 0, 0, 0, 0, 2, 2001], 30000, 990), 30000)

    def test_summary(self):
        recs = beat(10_000_000, (98, 600, [0, 90, 8, 0, 0, 0, 0, 0])) + \
            [{"kind": "log", "module": "engine", "msg": "out 90 48 7f", "args": [], "t_us": 1}] + \
            beat(20_000_000, (2, 4200, [0, 0, 0, 1, 0, 1, 0, 0]), stack_unused=900, lost=3)
        beats = sl.read_metrics(recs)
        self.assertEqual(len(beats), 2)
        self.assertEqual(beats[1]["stacks"]["smc_ctl"], {"unused": None, "size": 3072})
        s = sl.summarize_metrics(beats)
        usb = s["latency"]["usb"]
        self.assertEqual((usb["n"], usb["max"], usb["p50"], usb["p99"]), (100, 4200, 500, 2000))  # the 99th of 100 is in [1, 2) ms
        self.assertEqual(usb["buckets"], [0, 90, 8, 1, 0, 1, 0, 0])
        self.assertEqual(s["latency"]["core"]["n"], 0)
        self.assertEqual(s["stacks"]["smc_usb"], {"unused": 900, "size": 2048})
        self.assertEqual(s["log"], {"lost": 3, "ring_max": 900, "ring": 8192})
        self.assertEqual(s["traffic"]["events"], 100)
        self.assertEqual(s["traffic"]["midi_queue_max"], 3)
        self.assertEqual(s["clock"], {"resyncs": 0, "drift_min_us": 120, "drift_max_us": 980})
        self.assertEqual(s["span_s"], 10.0)
        self.assertEqual(s["load"], {"max": 1, "mean": 1})
        self.assertEqual(s["adc"], {"samples": 80000, "deferred": 14, "lost": 2, "timeouts": 0, "steps_max": 5,
                                    "fader_every_max_us": 2000})
        self.assertEqual(s["failed"], [])
        self.assertIn("CPU load, per mille: 1 over the session", sl.metrics_text(s))
        self.assertIn("checks: ok", sl.metrics_text(s))

    def test_checks(self):
        recs = beat(1, (100, 9000, [0, 50, 0, 0, 0, 48, 2, 0]), stack_unused=100, adc_timeouts=4)
        s = sl.summarize_metrics(sl.read_metrics(recs))
        self.assertEqual(s["failed"], ["fader ADC: 4 conversions never finished",
                                       "input to USB p99 <= 9000 us > NFR-01's 5000 us",
                                       "stack smc_usb: only 100 bytes free at the deepest point sampled"])
        with tempfile.TemporaryDirectory() as tmp:
            os.mkdir(os.path.join(tmp, "session"))
            with open(os.path.join(tmp, "session", "device.jsonl"), "w") as f:
                f.writelines(json.dumps(r) + "\n" for r in recs)
            p = subprocess.run([sys.executable, "-I", SMCLOG, "metrics", "--check", os.path.join(tmp, "session")],
                               capture_output=True, text=True, timeout=20)
            self.assertEqual(p.returncode, 1, p.stderr)
            self.assertIn("FAIL input to USB p99 <= 9000 us", p.stdout)
            p = subprocess.run([sys.executable, "-I", SMCLOG, "metrics", "--json", os.path.join(tmp, "session")],
                               capture_output=True, text=True, timeout=20)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual(json.loads(p.stdout)["heartbeats"], 1)

    @unittest.skipUnless(os.access(DEVHOST, os.X_OK), "build/host/devhost not built (make -C firmware test)")
    def test_heartbeat_of_the_stand_in(self):
        """METRICS_NOW on devhost: injected events show up in the latency, traffic and log parts."""
        import tty
        master, slave = os.openpty()
        tty.setraw(slave)
        dev = subprocess.Popen([DEVHOST, "20"], stdin=master, stdout=master)
        fd = sl.open_serial(os.ttyname(slave))
        try:
            recs = []
            link = sl.Link(fd, sl.Decoder(sl.make_dictionary(DEVHOST)), on_record=recs.append)
            link.call(sl.CMDS["subscribe"])
            self.assertEqual(link.call(sl.CMDS["inject"], sl.inject_payload(["on"]))["inject"], "ok")
            r = link.call(sl.CMDS["inject"], sl.inject_payload(["button:0:press", "button:0:release",
                                                                "fader:0:900"]))
            self.assertEqual(r["accepted"], 3)
            time.sleep(0.1)  # a round of the control path, the stand-in USB takes the packets
            link.call(sl.CMDS["metrics"])
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and not any(x.get("msg", "").startswith("log: ") and
                                                          x.get("module") == "metrics" for x in recs):
                recs += link.read(0.2)[0]
            beats = sl.read_metrics(recs)
            self.assertEqual(len(beats), 1, [x.get("msg") for x in recs])
            b = beats[0]
            self.assertEqual(b["seq"], 1)
            core, usb = b["latency"]["core"], b["latency"]["usb"]
            self.assertEqual(core["n"], 3)
            self.assertEqual(sum(core["buckets"]), 3)
            self.assertLess(core["max"], 1_000_000)
            self.assertEqual(usb["n"], 3)  # note on, note off, the CC
            self.assertLessEqual(core["min"], usb["min"])
            self.assertEqual(b["traffic"]["events"], 3)
            self.assertEqual(b["traffic"]["midi_out"], 3)
            self.assertEqual(b["traffic"]["midi_queue_max"], 3)
            self.assertEqual(b["log"]["lost"], 0)
            self.assertNotIn("scan_steps", b)  # no scan, clock, heap or stacks on the host
            self.assertNotIn("heap", b)
            self.assertEqual(b["stacks"], {})
            s = sl.summarize_metrics(beats)
            self.assertEqual(s["failed"], [])
        finally:
            os.close(fd)
            dev.kill()
            dev.wait()
            os.close(master)
            os.close(slave)


def devhost(*args):
    """devhost on a pseudo-terminal: (process, link, records, close)."""
    import tty
    master, slave = os.openpty()
    tty.setraw(slave)
    dev = subprocess.Popen([DEVHOST] + list(args), stdin=master, stdout=master)
    fd = sl.open_serial(os.ttyname(slave))
    recs = []
    link = sl.Link(fd, sl.Decoder(sl.make_dictionary(DEVHOST)), on_record=recs.append)

    def close():
        os.close(fd)
        if dev.poll() is None:
            dev.kill()
        dev.wait()
        os.close(master)
        os.close(slave)
    return dev, link, recs, close


def read_until(link, recs, pred, seconds=5):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline and not any(pred(r) for r in recs):
        recs += link.read(0.2)[0]


def boot_done(r):
    return r.get("module") == "boot" and r.get("msg", "").startswith("boot done")


class Boot(unittest.TestCase):
    """The boot journal (LOG-11) read back and summed up."""

    def test_symbolize(self):
        funcs = [(0x100, 0x20, "a"), (0x120, 0, "b"), (0x200, 0x10, "c")]
        self.assertEqual(sl.symbolize(0x100, funcs), "a")
        self.assertEqual(sl.symbolize(0x11E, funcs), "a+0x1e")
        self.assertEqual(sl.symbolize(0x120, funcs), "b")  # size 0: only its first address
        self.assertIsNone(sl.symbolize(0x121, funcs))
        self.assertIsNone(sl.symbolize(0xFF, funcs))
        self.assertEqual(sl.symbolize(0x20F, funcs), "c+0xf")
        self.assertIsNone(sl.symbolize(0x210, funcs))
        self.assertEqual(sl.elf_functions(SMCLOG), [])  # not an ELF

    @unittest.skipUnless(os.path.exists(os.path.join(ROOT, "build", "app", "app.elf")), "build/app/app.elf missing")
    def test_functions_of_the_image(self):
        funcs = sl.elf_functions(os.path.join(ROOT, "build", "app", "app.elf"))
        names = {f[2]: f for f in funcs}
        self.assertIn("input_br25_time_us", names)  # not inlined (TIMER3 check of build_app.sh)
        start = names["smclog_write"][0]
        self.assertEqual(sl.symbolize(start + 2, funcs), "smclog_write+0x2")

    @unittest.skipUnless(os.access(DEVHOST, os.X_OK), "build/host/devhost not built (make -C firmware test)")
    def test_reset_with_a_crash_on_the_stand_in(self):
        """devhost on one keep file: a run, a crash (SIGUSR1), the next run reports both."""
        with tempfile.TemporaryDirectory() as tmp:
            keep = os.path.join(tmp, "keep.bin")
            dev, link, recs, close = devhost("20", "--keep", keep)
            try:
                link.call(sl.CMDS["subscribe"])
                read_until(link, recs, boot_done)
                first = sl.read_boot(recs)
                self.assertEqual(len(first), 1)
                j = first[0]
                self.assertEqual((j["boot"], j["previous"], j["crash"], j["recent"]), (1, None, None, None))
                self.assertEqual((j["reset"]["text"], j["version"]), ("host start", "devhost"))
                self.assertEqual([m["what"] for m in j["marks"]], ["core", "link"])
                link.call(sl.CMDS["inject"], sl.inject_payload(["on"]))
                link.call(sl.CMDS["inject"], sl.inject_payload(["button:0:press", "button:0:release"]))
                time.sleep(0.1)
                os.kill(dev.pid, signal.SIGUSR1)  # the stand-in's crash
                self.assertEqual(dev.wait(timeout=5), 70)
            finally:
                close()

            dev, link, recs, close = devhost("20", "--keep", keep)
            try:
                link.call(sl.CMDS["subscribe"])
                read_until(link, recs, boot_done)
                self.assertEqual(link.call(sl.CMDS["boot"])["status"], "ok")
                read_until(link, recs, lambda r: r.get("msg", "").startswith("boot done") and
                           r is not next(x for x in recs if boot_done(x)))
            finally:
                close()
        self.assertNotIn("gap", [r["kind"] for r in recs])
        journals = sl.read_boot(recs)
        self.assertEqual([j["again"] for j in journals], [False, True])
        j = journals[0]
        self.assertEqual(j["boot"], 2)
        self.assertEqual(j["previous"], {"heartbeat": 0})  # killed before its first heartbeat
        c = j["crash"]
        self.assertEqual((c["boot"], c["cause_name"], c["task"], c["build_tag"]), (1, "signal (devhost)", "devhost",
                                                                                     j["build_tag"]))
        self.assertEqual((c["regs"]["R0"], c["regs"]["R15"], c["regs"]["RETI"], c["regs"]["PRP_WR_LIMIT_MSG"]),
                         (0xC0DE0000, 0xC0DE000F, 0xC0DE0010, 0xC0DE001B))
        self.assertEqual(c["trace"][0], 0xC0DE1000)
        self.assertEqual(c["stack"][0], (0x20000, 0x5A000000))
        self.assertEqual(c["stack"][-1], (0x20000 + 13 * 4, 0x5A00000D))
        self.assertGreater(j["recent"], 5)
        self.assertEqual(len(j["past"]), j["recent"])
        msgs = [r["msg"] for r in j["past"]]
        self.assertTrue(msgs[0].startswith("boot 1 since the RAM was lost"))  # the previous run, from its start
        self.assertTrue(any(m.startswith("injection on") or "inject" in m for m in msgs), msgs)
        again = journals[1]
        self.assertEqual((again["boot"], again["past"], again["crash"]["task"]), (2, [], "devhost"))
        self.assertEqual([m["what"] for m in again["marks"]], ["core", "link"])
        text = sl.boot_text(j)
        self.assertIn("CRASH in boot 1 at about", text)
        self.assertIn("RETI 0xc0de0010", text)
        self.assertIn("previous run: its latest %d log records:" % j["recent"], text)
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "device.jsonl")
            with open(path, "w") as f:
                f.writelines(json.dumps(r) + "\n" for r in recs)
            p = subprocess.run([sys.executable, "-I", SMCLOG, "boot", path], capture_output=True, text=True, timeout=20)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertIn("boot 2 since the RAM was lost: reset cause 0x0 (host start)", p.stdout)
            self.assertIn("(journal again on request)", p.stdout)
            p = subprocess.run([sys.executable, "-I", SMCLOG, "boot", "--json", path], capture_output=True, text=True,
                               timeout=20)
            self.assertEqual(json.loads(p.stdout)[0]["crash"]["task"], "devhost")


if __name__ == "__main__":
    unittest.main()

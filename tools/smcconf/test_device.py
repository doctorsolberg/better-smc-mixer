"""Tests of smcconf info, pull and push (the configuration protocol's host side, devlink.py; docs/link.md,
"Configuration") against build/host/devhost on a pseudo-terminal, and over SysEx on its config port
(--sysex-fd, a socket; docs/link.md, "Over SysEx"): the controller's portable code with
the command link, the configuration protocol and the core (made by make -C firmware test). Also
against build/host/test_link --serve, a device without the protocol. With devhost --flash, the
autosave and the load at start (store/cfgstore.h, on a model of NOR flash in a file). Skipped without
devhost.

Run: python3 -I tools/smcconf/test_device.py
"""
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import tty
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import codec  # noqa: E402
import devlink  # noqa: E402

smclog = devlink.smclog
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
DEVHOST = os.environ.get("SMC_DEVHOST") or os.path.join(ROOT, "build", "host", "devhost")
TEST_LINK = os.environ.get("SMC_TEST_LINK") or os.path.join(ROOT, "build", "host", "test_link")
SMCCONF = os.path.join(HERE, "smcconf.py")
FACTORY = os.path.join(ROOT, "config", "factory.json")
TEST = os.path.join(ROOT, "config", "test.json")
UUID = "0f1e2d3c-4b5a-4978-8695-a4b3c2d1e0f9"


class StandIn:
    """A program on a pseudo-terminal's master side; the test keeps the terminal's side open, as the
    device stays while the tool opens and closes its port."""

    def __init__(self, argv, pass_fds=()):
        self.master, self.slave = os.openpty()
        tty.setraw(self.slave)
        self.port = os.ttyname(self.slave)
        self.proc = subprocess.Popen(argv, stdin=self.master, stdout=self.master, pass_fds=pass_fds)
        time.sleep(0.1)

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()
        os.close(self.master)
        os.close(self.slave)


def smcconf(*args, pass_fds=()):
    return subprocess.run([sys.executable, "-I", SMCCONF] + list(args), capture_output=True, text=True,
                          timeout=60, pass_fds=pass_fds)


def read_text(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


@unittest.skipUnless(os.access(DEVHOST, os.X_OK), "build/host/devhost not built (make -C firmware test)")
class DeviceTest(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.midi = os.path.join(self.tmp.name, "midi.bin")
        open(self.midi, "wb").close()
        self.dev = StandIn([DEVHOST, "60", "--midi", self.midi])

    def tearDown(self):
        self.dev.close()
        self.tmp.cleanup()

    def path(self, name):
        return os.path.join(self.tmp.name, name)

    def with_id(self, src, revision):
        """src with an ID and a revision, as a file."""
        with open(src, encoding="utf-8") as f:
            obj = json.load(f)
        obj["id"] = UUID
        obj["revision"] = revision
        out = self.path("pushed.json")
        with open(out, "w", encoding="utf-8") as f:
            json.dump(obj, f)
        return out

    def test_info(self):
        r = smcconf("info", "--port", self.dev.port)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("configuration protocol 4, binary form %d, schema %d" % (codec.S.binary_version, codec.S.version),
                      r.stdout)
        self.assertIn("idle; running: factory, revision 0, ID none; storage: none, saved generation 0", r.stdout)

    def test_state_select_watch(self):
        # P-2, P-3 over the CDC: the state, a selection, the events
        r = smcconf("state", "--port", self.dev.port)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), "preset 1 page 1; bluetooth on; 0 touches, the last: none; storage none; "
                                           "revision 0, ID none")
        r = smcconf("select", "3", "2", "--port", self.dev.port)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(r.stdout.startswith("preset 3 page 2;"), r.stdout)
        r = smcconf("select", "1", "--port", self.dev.port)  # the preset alone: its last page
        self.assertTrue(r.stdout.startswith("preset 1 page 1;"), r.stdout + r.stderr)
        r = smcconf("select", "5", "--port", self.dev.port)
        self.assertEqual(r.returncode, 2)
        r = smcconf("watch", "--port", self.dev.port, "--seconds", "0.3")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.splitlines(), ["event   0: preset 1 page 1; bluetooth on; 0 touches, the last: none; "
                                                 "storage none; revision 0, ID none"])

    def test_sent(self):
        """P-5: what the mixer sent, with the control, as SENT records and by CFG_SENT; and the same
        messages on the MIDI port."""
        with devlink.Device(self.dev.port) as d:
            first, none = d.sent_since()
            self.assertEqual(none, [])
            d.watch(False, sent=True)
            for args in (["on"], ["button:0:press"], ["button:0:release"], ["off"]):
                r = d.link.call(smclog.CMDS["inject"], smclog.inject_payload(args))
                self.assertEqual((r["status"], r.get("inject")), ("ok", "ok"))
            lost, got = 0, []
            deadline = time.monotonic() + 2
            while len(got) < 2 and time.monotonic() < deadline:
                l, e = d.sent(0.5)
                lost += l
                got += e
            self.assertEqual(lost, 0)
            self.assertEqual([e["n"] for e in got], [first, first + 1])
            m1 = {"preset": 0, "page": 0, "control": 16, "layer": 0}  # M1: group 2, strip 1
            self.assertEqual([e["source"] for e in got], [m1, m1])
            self.assertEqual(d.events(0.1), [])  # the panel's EVENTs stay off
            d.watch(False)
            again_first, again = d.sent_since(first)
            self.assertEqual((again_first, again), (first, got))
            self.assertEqual(d.sent_since()[0], first + 2)
            r = smcconf("watch", "--port", self.dev.port, "--seconds", "0.3", "--sent")
            self.assertEqual(r.returncode, 0, r.stderr)
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline and os.path.getsize(self.midi) < 6:
            time.sleep(0.02)
        with open(self.midi, "rb") as f:
            self.assertEqual(f.read()[:6], got[0]["msg"] + got[1]["msg"])

    def test_pull_factory(self):
        """The running configuration at start is the factory one: config/factory.json exactly."""
        out = self.path("pulled.json")
        r = smcconf("pull", "--port", self.dev.port, "-o", out)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("factory, revision 0", r.stderr)
        self.assertEqual(read_text(out), read_text(FACTORY))
        r = smcconf("pull", "--port", self.dev.port, "--binary", "-o", self.path("pulled.bin"))
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(self.path("pulled.bin"), "rb") as f:
            self.assertEqual(f.read(), codec.encode(codec.load(FACTORY)))

    def test_push_then_pull(self):
        src = self.with_id(TEST, 7)
        r = smcconf("push", src, "--port", self.dev.port)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("revision 1: applied, read back the same", r.stdout)
        r = smcconf("info", "--port", self.dev.port)
        self.assertIn("running: received, revision 1, ID %s" % UUID.replace("-", ""), r.stdout)
        out = self.path("pulled.json")
        r = smcconf("pull", "--port", self.dev.port, "-o", out)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(codec.load(out), dict(codec.load(src), revision=1))

        # the core runs on it: M1 of the test configuration is note 24 on channel 1, sent to the host
        with devlink.Device(self.dev.port) as d:
            for args in (["on"], ["button:0:press"]):
                r = d.link.call(smclog.CMDS["inject"], smclog.inject_payload(args))
                self.assertEqual((r["status"], r.get("inject")), ("ok", "ok"))
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline and os.path.getsize(self.midi) < 3:
            time.sleep(0.02)
        with open(self.midi, "rb") as f:
            self.assertEqual(f.read()[:3], bytes([0x90, 24, 0x7F]))

        # a binary file pushes the same way; the factory configuration back
        blob = self.path("factory.bin")
        self.assertEqual(smcconf("encode", FACTORY, "-o", blob).returncode, 0)
        r = smcconf("push", blob, "--port", self.dev.port, "--no-verify")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), "pushed %d bytes, revision 2: applied" % os.path.getsize(blob))
        r = smcconf("pull", "--port", self.dev.port, "-o", out)
        self.assertEqual(codec.load(out), dict(codec.load(FACTORY), revision=2))
        self.assertIn("received, revision 2", r.stderr)

    def test_refused_and_interrupted(self):
        """A form the device refuses, an upload cut off: the running configuration stays as it was."""
        good = codec.encode(codec.load(self.with_id(TEST, 3)))
        with devlink.Device(self.dev.port) as d:
            bad = b"X" + good[1:]
            with self.assertRaises(devlink.DeviceError) as e:
                d.push(bad)
            self.assertIn("cfg-put-commit: magic (byte 0)", str(e.exception))
            bad = good[:-1] + bytes([good[-1] ^ 0x40])
            with self.assertRaises(devlink.DeviceError) as e:
                d.push(bad)
            self.assertIn("cfg-put-commit: crc", str(e.exception))
            # cut off after one chunk, the port closed
            st, _ = d.call("cfg-put-begin", struct.pack("<II", len(good), 0))
            self.assertEqual(st, "ok")
            st, _ = d.call("cfg-put-chunk", bytes(4) + good[:128])
            self.assertEqual(st, "ok")
            self.assertEqual(d.info()["state"], "put")
        out = self.path("pulled.json")
        r = smcconf("pull", "--port", self.dev.port, "-o", out)  # a new BEGIN drops the upload
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(read_text(out), read_text(FACTORY))
        with devlink.Device(self.dev.port) as d:
            i = d.info()
            self.assertEqual((i["state"], i["source"], i["revision"], i["id"]), ("get", "factory", 0, None))
            st, _ = d.call("cfg-put-chunk", bytes(4) + good[:128])
            self.assertEqual(st, "no transfer")
            self.assertEqual(d.push(good)["revision"], 1)  # and a whole one goes through
            # more than the device's buffer takes
            limit = d.info()["max_length"]
            self.assertEqual(limit, 16384)
            st, _ = d.call("cfg-put-begin", struct.pack("<II", limit + 1, 0))
            self.assertEqual(st, "size")

    def test_refused_here(self):
        """A file that is not valid is refused before the device is asked anything."""
        bad = self.path("bad.json")
        with open(bad, "w") as f:
            f.write('{"schemaVersion": 1, "presets": [{"channel": 17}]}')
        r = smcconf("push", bad, "--port", self.dev.port)
        self.assertEqual(r.returncode, 1)
        self.assertIn("presets[0].channel", r.stderr)
        self.assertEqual(smcconf("push", self.path("missing.json"), "--port", self.dev.port).returncode, 2)


@unittest.skipUnless(os.access(DEVHOST, os.X_OK), "build/host/devhost not built (make -C firmware test)")
@unittest.skipUnless(os.access(DEVHOST, os.X_OK), "build/host/devhost not built (make -C firmware test)")
class SysexTest(unittest.TestCase):
    """The command link over SysEx: devhost's config port on one end of a socket pair, the host's
    MIDI port (hostmidi fd:N) on the other."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.host, dev = socket.socketpair()
        self.dev = StandIn([DEVHOST, "60", "--sysex-fd", str(dev.fileno())], pass_fds=(dev.fileno(),))
        dev.close()
        self.midi = "fd:%d" % self.host.fileno()

    def tearDown(self):
        self.dev.close()
        self.host.close()
        self.tmp.cleanup()

    def test_info_pull_push(self):
        fds = (self.host.fileno(),)
        r = smcconf("info", "--midi", self.midi, pass_fds=fds)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("idle; running: factory, revision 0, ID none", r.stdout)
        out = os.path.join(self.tmp.name, "pulled.json")
        r = smcconf("pull", "--midi", self.midi, "-o", out, pass_fds=fds)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(read_text(out), read_text(FACTORY))
        with open(TEST, encoding="utf-8") as f:
            obj = json.load(f)
        obj["id"], obj["revision"] = UUID, 9
        src = os.path.join(self.tmp.name, "pushed.json")
        with open(src, "w", encoding="utf-8") as f:
            json.dump(obj, f)
        r = smcconf("push", src, "--midi", self.midi, pass_fds=fds)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("revision 1: applied, read back the same", r.stdout)
        # the CDC link sees the same configuration: one configuration protocol for the device
        r = smcconf("info", "--port", self.dev.port)
        self.assertIn("running: received, revision 1", r.stdout)

    def test_link_over_sysex(self):
        with devlink.hostmidi.open_port(self.midi) as port:
            link = smclog.SysexLink(port)
            r = link.call(smclog.CMDS["info"])
            self.assertEqual(r["status"], "ok")
            self.assertFalse("log" in r.get("caps", []), r)  # no log stream over SysEx
            r = link.call(smclog.CMDS["subscribe"])
            self.assertEqual(r["status"], "unavailable")
            # other SysEx, channel messages and real-time around ours: answered all the same
            port.send(b"\xF0\x7E\x7F\x06\x01\xF7\x90\x40\x7F\xF8")
            r = link.call(smclog.CMDS["ping"], b"\x80\xFF\x00")
            self.assertEqual((r["status"], r["echo"]), ("ok", "80ff00"))
            # a broken frame gets no reply; the next one does
            msg = bytearray(smclog.sysex_command(1, smclog.CMDS["ping"]))
            msg[6] ^= 1
            port.send(bytes(msg))
            self.assertEqual(port.receive(0.2), [])
            r = link.call(smclog.CMDS["ping"])
            self.assertEqual(r["status"], "ok")
            # the same seq again: the same reply, as a retry (FR-CFG-03)
            again = smclog.sysex_command(link.seq, smclog.CMDS["ping"])
            port.send(again)
            back = port.receive(1.0)
            self.assertEqual(len(back), 1)
            self.assertEqual(smclog.sysex_record(back[0])[1], link.seq)
            self.assertEqual(link.foreign, 0)


@unittest.skipUnless(os.access(DEVHOST, os.X_OK), "build/host/devhost not built (make -C firmware test)")
class StockTest(unittest.TestCase):
    """devhost --boot: going back to M-Vave's firmware (STOCK) on a synthetic installed flash."""

    def test_back(self):
        with tempfile.TemporaryDirectory() as tmp:
            vec = os.path.join(tmp, "both.bin")
            subprocess.run([sys.executable, "-I", os.path.join(ROOT, "firmware", "tests", "bootsel_vec.py"), vec], check=True)
            with open(vec, "rb") as f:
                stock, installed = f.read(0x80000), f.read(0x80000)
            boot = os.path.join(tmp, "boot.bin")
            with open(boot, "wb") as f:
                f.write(installed)
            dev = StandIn([DEVHOST, "60", "--boot", boot])
            try:
                with devlink.Device(dev.port) as d:
                    info = d.link.call(smclog.CMDS["info"])
                    self.assertIn("stock", info["caps"])
                    r = d.link.call(smclog.CMDS["stock"], b"\x00")
                    self.assertEqual((r["status"], r["stock"], r["pointer"], r["tries"]), ("ok", "ok", 0x41000, 0))
                    r = d.link.call(smclog.CMDS["stock"], b"\x01BACK")
                    self.assertEqual((r["status"], r["stock"], r["pointer"], r["tries"]), ("ok", "ok", 0x1000, 1))
                # the reset: devhost runs itself again on the same flash, and as sector 0 now points
                # at M-Vave's area, ours does not start: it exits
                self.assertEqual(dev.proc.wait(timeout=3), 0)
            finally:
                dev.close()
            with open(boot, "rb") as f:
                after = f.read()
            self.assertEqual(after[:0x1000], stock[:0x1000])  # M-Vave's sector 0, byte for byte
            self.assertEqual(after[0x1000:], installed[0x1000:])
        # without --boot it is not offered
        dev = StandIn([DEVHOST, "60"])
        try:
            with devlink.Device(dev.port) as d:
                self.assertNotIn("stock", d.link.call(smclog.CMDS["info"])["caps"])
                self.assertEqual(d.link.call(smclog.CMDS["stock"], b"\x00")["status"], "unavailable")
        finally:
            dev.close()


class FlashTest(unittest.TestCase):
    """devhost --flash: a received configuration is saved after the quiet pause and runs after a reset."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.flash = os.path.join(self.tmp.name, "flash.bin")
        self.dev = StandIn([DEVHOST, "60", "--flash", self.flash])

    def tearDown(self):
        self.dev.close()
        self.tmp.cleanup()

    def pushed(self, revision, save="auto", name="pushed.json"):
        with open(TEST, encoding="utf-8") as f:
            obj = json.load(f)
        obj["id"] = UUID
        obj["revision"] = revision
        if save != "manual":
            obj.setdefault("global", {})["save"] = save
        out = os.path.join(self.tmp.name, name)
        with open(out, "w", encoding="utf-8") as f:
            json.dump(obj, f)
        return out

    def test_saved_across_reset(self):
        r = smcconf("info", "--port", self.dev.port)
        self.assertIn("running: factory, revision 0, ID none; storage: clean, saved generation 0", r.stdout)
        src = self.pushed(7)
        t0 = time.monotonic()
        r = smcconf("push", src, "--port", self.dev.port, "--wait-save")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("revision 1: applied, read back the same, saved (generation 1)", r.stdout)
        self.assertGreaterEqual(time.monotonic() - t0, 2.9)  # not before the quiet pause
        r = smcconf("info", "--port", self.dev.port)
        self.assertIn("running: received, revision 1", r.stdout)
        self.assertIn("storage: clean, saved generation 1", r.stdout)

        # a reset (devhost runs itself again on the same flash file): the saved configuration
        with devlink.Device(self.dev.port) as d:
            r = d.link.call(smclog.CMDS["reset"], bytes([smclog.RESET_HOW["soft"]]))
            self.assertEqual((r["status"], r.get("reset")), ("ok", "ok"))
        time.sleep(0.6)
        r = smcconf("info", "--port", self.dev.port)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("running: stored, revision 1, ID %s; storage: clean, saved generation 1"
                      % UUID.replace("-", ""), r.stdout)
        out = os.path.join(self.tmp.name, "pulled.json")
        r = smcconf("pull", "--port", self.dev.port, "-o", out)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("stored, revision 1", r.stderr)
        self.assertEqual(codec.load(out), dict(codec.load(src), revision=1))

        # a new process on the flash file, as after a power cycle: the same
        self.dev.close()
        self.dev = StandIn([DEVHOST, "60", "--flash", self.flash])
        r = smcconf("info", "--port", self.dev.port)
        self.assertIn("running: stored, revision 1", r.stdout)

    def test_save_edit_revert(self):
        """Q-26 and P-1: saved by hand (the default), live edits, and back to what it starts with."""
        src = self.pushed(3, save="manual")
        r = smcconf("push", src, "--port", self.dev.port)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = smcconf("info", "--port", self.dev.port)
        self.assertIn("storage: unsaved, saved generation 0", r.stdout)
        time.sleep(3.5)
        r = smcconf("push", src, "--port", self.dev.port, "--wait-save")
        self.assertEqual(r.returncode, 1)
        self.assertIn("use --save", r.stderr)
        r = smcconf("save", "--port", self.dev.port)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("saved: generation 1, revision 2", r.stdout)
        r = smcconf("save", "--port", self.dev.port)  # nothing differs: at once
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("saved: generation 1, revision 2", r.stdout)

        # Two SETs advance twice, keeping the running ID despite the file's unrelated metadata.
        m = codec.load(src)
        m["presets"][0]["pages"][1]["strips"][0]["encoder"]["normal"]["number"] = 77
        m["presets"][1]["channel"] = 11
        m["revision"] = 42
        m["id"] = bytes(16)
        edited = os.path.join(self.tmp.name, "edited.json")
        with open(edited, "w", encoding="utf-8") as f:
            f.write(codec.dumps(codec.to_json(m)))
        r = smcconf("edit", edited, "--port", self.dev.port)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("edited: 2 commands", r.stdout)
        self.assertIn("revision 4, without a restart; read back the same", r.stdout)
        r = smcconf("info", "--port", self.dev.port)
        self.assertIn("running: received, revision 4, ID %s; storage: unsaved" % UUID.replace("-", ""), r.stdout)
        r = smcconf("edit", edited, "--port", self.dev.port)  # the same: nothing to send
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("edited: 0 commands", r.stdout)
        self.assertIn("revision 4, without a restart; read back the same", r.stdout)

        # revert: the saved one again
        r = smcconf("revert", "--port", self.dev.port)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("reverted: stored, revision 5", r.stdout)
        out = os.path.join(self.tmp.name, "pulled.json")
        r = smcconf("pull", "--port", self.dev.port, "-o", out)
        self.assertEqual(codec.load(out), dict(codec.load(src), revision=5))
        r = smcconf("info", "--port", self.dev.port)
        self.assertIn("storage: clean, saved generation 1", r.stdout)

        # edit and save in one go; after a reset it is there
        r = smcconf("edit", edited, "--port", self.dev.port, "--save")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("saved (generation 2)", r.stdout)
        self.dev.close()
        self.dev = StandIn([DEVHOST, "60", "--flash", self.flash])
        r = smcconf("info", "--port", self.dev.port)
        self.assertIn("running: stored, revision 7", r.stdout)

    def test_touch_holds_the_save(self):
        """While controls are touched the save waits; it happens 3 s after the last touch."""
        r = smcconf("push", self.pushed(9), "--port", self.dev.port)
        self.assertEqual(r.returncode, 0, r.stderr)
        with devlink.Device(self.dev.port) as d:
            r = d.link.call(smclog.CMDS["inject"], smclog.inject_payload(["on"]))
            self.assertEqual(r.get("inject"), "ok")
            for _ in range(5):  # a touch a second for 5 s
                r = d.link.call(smclog.CMDS["inject"], smclog.inject_payload(["encoder:0:+1"]))
                self.assertEqual(r.get("inject"), "ok")
                time.sleep(1.0)
            i = d.info()
            self.assertEqual((i["store"], i["generation"]), ("waiting", 0))
            t0 = time.monotonic()
            i = d.wait_saved(0, timeout=10)
            self.assertEqual(i["generation"], 1)
            self.assertGreaterEqual(time.monotonic() - t0, 1.5)


@unittest.skipUnless(os.access(TEST_LINK, os.X_OK), "build/host/test_link not built (make -C firmware test)")
class NoProtocolTest(unittest.TestCase):

    def test_no_protocol(self):
        dev = StandIn([TEST_LINK, "--serve", "30"])
        try:
            for cmd in (["info"], ["pull"], ["push", TEST]):
                r = smcconf(*cmd, "--port", dev.port)
                self.assertEqual(r.returncode, 1, cmd)
                self.assertIn("no configuration protocol", r.stderr)
        finally:
            dev.close()

    def test_no_device(self):
        master, slave = os.openpty()  # a port with nothing behind it
        try:
            tty.setraw(slave)
            r = smcconf("info", "--port", os.ttyname(slave), "--timeout", "0.2")
            self.assertEqual(r.returncode, 1)
            self.assertIn("no reply to info after 3 tries", r.stderr)
        finally:
            os.close(master)
            os.close(slave)


if __name__ == "__main__":
    unittest.main(verbosity=1)

"""Tests of the scenario runner (run: python3 -I tools/smcsim/test_smcsim.py; make -C firmware test).

They need the simulator: build/host/smcsim (make -C firmware sim). The device runner's tests also
need build/host/devhost (the controller's portable code behind the command link) and
build/host/test_link, each run behind a pseudo-terminal."""
import ctypes
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
import tty
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import smcsim  # noqa: E402

SCENARIOS = os.path.join(smcsim.ROOT, "firmware", "tests", "scenarios")
DEVHOST = os.path.join(smcsim.ROOT, "build", "host", "devhost")
LINK_BIN = os.path.join(smcsim.ROOT, "build", "host", "test_link")
SMCSIM = os.path.join(HERE, "smcsim.py")


def scenario(tmp, lines, name="s.jsonl"):
    path = os.path.join(tmp, name)
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    return path


class Runner(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def test_failures_name_the_line(self):
        path = scenario(self.tmp, [
            '# a comment',
            '{"tap": "m1"}',
            '{"expect_midi": ["90 18 7E"]}',
            '{"expect_led": "m1", "state": "off"}',
            '{"expect_state": {"page": 3}}',
            '{"expect_log": "nothing like this"}',
        ])
        ok, failures, _ = smcsim.run_scenario(path)
        self.assertFalse(ok)
        self.assertEqual(len(failures), 5)
        self.assertIn("s.jsonl:3: MIDI", failures[0])
        self.assertIn("90 18 7F", failures[0])
        self.assertIn("s.jsonl:4: LED m1 is on, want off", failures[1])
        self.assertIn("s.jsonl:5: page is 1, want 3", failures[2])
        self.assertIn("s.jsonl:6: no log record", failures[3])
        self.assertIn("s.jsonl: end: notes left on: channel 1 note 24", failures[4])

    def test_notes_left_on(self):
        self.assertEqual(smcsim.notes_left_on(["90 18 7F", "91 18 7F", "B0 18 7F"]), [(0, 24), (1, 24)])
        self.assertEqual(smcsim.notes_left_on(["90 18 7F", "90 18 00", "91 3C 40", "81 3C 00"]), [])
        path = scenario(self.tmp, ['{"tap": "m1"}', '{"tap": "m1"}', '{"expect_midi": ["90 18 7F", "90 18 00"]}'])
        ok, failures, _ = smcsim.run_scenario(path)
        self.assertTrue(ok, failures)

    def test_bad_steps(self):
        for lines, text in [
            (['{"tap": "nope"}'], "no button named 'nope'"),
            (['{"tap": "m1"}', '{"set": {"pickup": "jump"}}'], "before any input"),
            (['{"set": {"colour": "red"}}'], "unknown setting"),
            (['{"jump": 1}'], "unknown step"),
            (['{"midi_in": "B0 1"}'], "not a MIDI message"),
            (['{"fader": "fader1"}'], "needs 'to'"),
            (['{"fader": "fader1", "to": 200}'], "not 0-127"),
            (['not json'], "s.jsonl:1"),
        ]:
            path = scenario(self.tmp, lines)
            with self.assertRaises(smcsim.ScenarioError) as e:
                smcsim.run_scenario(path)
            self.assertIn(text, str(e.exception))

    def test_settings_reach_the_core(self):
        path = scenario(self.tmp, [
            '{"set": {"shift_mode": "latch"}}',
            '{"tap": "shift"}',
            '{"expect_state": {"shift": true}}',
        ])
        ok, failures, _ = smcsim.run_scenario(path)
        self.assertTrue(ok, failures)

    def test_config_goes_first(self):
        """The "config" setting replaces the whole configuration: it goes before the other settings
        of its step, and the simulator refuses it after another setting."""
        self.assertEqual(smcsim.set_commands({"note_off": "8n", "config": "factory"}),
                         ["set config factory", "set note_off 8n"])
        path = scenario(self.tmp, [
            '{"set": {"note_off": "8n", "config": "factory"}}',
            '{"tap": "r1"}',
            '{"expect_midi": ["90 38 7F", "80 38 00"]}',
        ])
        ok, failures, _ = smcsim.run_scenario(path)
        self.assertTrue(ok, failures)
        path = scenario(self.tmp, ['{"set": {"note_off": "8n"}}', '{"set": {"config": "factory"}}',
                                   '{"fader": "fader1", "to": 32}'])
        with self.assertRaises(smcsim.ScenarioError) as e:
            smcsim.run_scenario(path)
        self.assertIn("set config factory", str(e.exception))

    def test_timing(self):
        tr = smcsim.Translator(smcsim.sim_names(smcsim.DEFAULT_SIM))
        self.assertEqual(tr.commands({"tap": "shift"}), ["b 10000 44 1", "b 60000 44 0"])
        self.assertEqual(tr.commands({"turn": "enc2", "clicks": -2, "every_ms": 5}),
                         ["e 70000 1 -1", "e 75000 1 -1"])
        self.assertEqual(tr.commands({"fader": "fader1", "from_pos": 0, "to_pos": 100, "steps": 2}),
                         ["f 85000 0 0", "f 95000 0 50", "f 105000 0 100"])
        self.assertEqual(tr.commands({"wait_ms": 25}), ["t 115000", "t 125000", "t 130000"])
        self.assertEqual(tr.commands({"midi_in": "c0 05", "transport": "ble", "injected": True}),
                         ["m 140000 ble C0 05 00 inj"])

    def test_session_and_replay(self):
        d = os.path.join(self.tmp, "session")
        ok, failures, _ = smcsim.run_scenario(os.path.join(SCENARIOS, "pickup.jsonl"), session=d)
        self.assertTrue(ok, failures)
        for name in ("meta.json", "device.bin", "device.jsonl", "midi.jsonl", "markers.jsonl", "result.json"):
            self.assertTrue(os.path.exists(os.path.join(d, name)), name)
        with open(os.path.join(d, "meta.json")) as f:
            meta = json.load(f)
        self.assertEqual(meta["build_id"], smcsim.smclog.make_dictionary(smcsim.DEFAULT_SIM)["build_id"])
        with open(os.path.join(d, "midi.jsonl")) as f:
            first = json.loads(f.readline())
        self.assertEqual((first["hex"], first["msg"], first["dir"]), ("B0 14 64", "cc ch1 20 100", "rx"))

        ok, detail = smcsim.replay(d)
        self.assertTrue(ok, detail)
        self.assertIn("3 MIDI messages agree", detail)

        # the device's core sending something else is a failure (LOG-35)
        path = os.path.join(d, "device.jsonl")
        with open(path) as f:
            text = f.read()
        with open(path, "w") as f:
            f.write(text.replace("out b1 14 14", "out b1 14 15", 1))
        ok, detail = smcsim.replay(d)
        self.assertFalse(ok)
        self.assertIn("simulator B1 14 14, session B1 14 15", detail)

    def test_replay_uses_settings(self):
        d = os.path.join(self.tmp, "hybrid")
        ok, failures, _ = smcsim.run_scenario(os.path.join(SCENARIOS, "shift-hybrid.jsonl"), session=d)
        self.assertTrue(ok, failures)
        path = scenario(self.tmp, ['{"set": {"shift_mode": "latch"}}', '{"tap": "shift"}', '{"tap": "down"}',
                                   '{"fader": "fader1", "to": 3}'])
        ok, failures, _ = smcsim.run_scenario(path, session=d)
        self.assertTrue(ok, failures)
        ok, detail = smcsim.replay(d)  # latched Shift: the fader is on page 2, channel 2
        self.assertTrue(ok, detail)

    def test_corpus_runs_twice_the_same(self):
        a, b = os.path.join(self.tmp, "a"), os.path.join(self.tmp, "b")
        for d in (a, b):
            smcsim.run_scenario(os.path.join(SCENARIOS, "encoders.jsonl"), session=d)
        for name in ("device.bin", "midi.jsonl"):
            with open(os.path.join(a, name), "rb") as f1, open(os.path.join(b, name), "rb") as f2:
                self.assertEqual(f1.read(), f2.read(), name)


@unittest.skipUnless(os.access(DEVHOST, os.X_OK), "build/host/devhost not built (make -C firmware test)")
class DeviceRunner(unittest.TestCase):
    """smcsim device against stand-ins behind a pseudo-terminal (LOG-35 without hardware)."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.procs, self.fds = [], []

    def tearDown(self):
        for p in self.procs:
            p.kill()
            p.wait()
        for fd in self.fds:
            os.close(fd)

    def port(self, argv):
        """Starts a stand-in on a pseudo-terminal; returns the port's name."""
        master, slave = os.openpty()
        tty.setraw(slave)  # no echo of the device's bytes back to it
        self.fds += [master, slave]
        self.procs.append(subprocess.Popen(argv, stdin=master, stdout=master))
        return os.ttyname(slave)

    def device(self, port, binary, *args):
        return subprocess.run([sys.executable, "-I", SMCSIM, "device", "--port", port, "--binary", binary] + list(args),
                              capture_output=True, text=True, timeout=180)

    def test_corpus_on_the_stand_in(self):
        session = os.path.join(self.tmp, "sessions")
        p = self.device(self.port([DEVHOST, "120"]), DEVHOST, SCENARIOS, "--session", session)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        lines = p.stdout.splitlines()
        n = len([x for x in os.listdir(SCENARIOS) if x.endswith(".jsonl")])
        self.assertEqual(sum(line.startswith("ok   ") for line in lines), n, p.stdout)
        self.assertEqual(lines[-1], "smcsim device: %d scenarios, 0 failed, 0 skipped" % n)
        d = os.path.join(session, "note-off")
        with open(os.path.join(d, "meta.json")) as f:
            meta = json.load(f)
        self.assertEqual((meta["source"], meta["settings"]), ("device", {"config": "test", "note_off_p2": "8n"}))
        with open(os.path.join(d, "device.jsonl")) as f:
            recs = [json.loads(line) for line in f]
        self.assertTrue(recs[0]["msg"].startswith("core start:"))
        self.assertTrue(any("note off forms 0 1 0 0" in r.get("msg", "") for r in recs))
        self.assertTrue(any(r.get("msg") == "out 84 28 00" for r in recs))  # the 8n form on preset 2
        with open(os.path.join(d, "markers.jsonl")) as f:
            self.assertEqual(len(f.readlines()), 19)
        with open(os.path.join(d, "result.json")) as f:
            self.assertTrue(json.load(f)["ok"])
        self.assertGreater(os.path.getsize(os.path.join(d, "device.bin")), 0)

    def test_disagreement_is_caught(self):
        # the stand-in takes the configuration but ignores the other settings: preset 2 keeps 9n
        port = self.port([DEVHOST, "60", "--fault", "ignore-settings"])
        p = self.device(port, DEVHOST, os.path.join(SCENARIOS, "note-off.jsonl"))
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("FAIL note-off.jsonl", p.stdout)
        self.assertIn("the device and the simulator disagree at MIDI message", p.stdout)
        self.assertIn("device 94 28 00, simulator 84 28 00", p.stdout)
        self.assertNotIn("note: the device and the simulator agree", p.stdout)

    def test_select_and_refusals(self):
        # a selection goes to the device as CFG_SELECT (P-3), timed where the device applied it
        path = scenario(self.tmp, ['{"select": {"page": 2}}', '{"fader": "fader1", "to": 64}', '{"select": {"preset": 3}}',
                                   '{"fader": "fader1", "to": 32}'])
        p = self.device(self.port([DEVHOST, "60"]), DEVHOST, path)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("ok   s.jsonl", p.stdout)
        self.assertIn("0 failed, 0 skipped", p.stdout)
        # the hardware-test variant has no core: refused before anything is injected
        p = self.device(self.port([LINK_BIN, "--serve", "30"]), LINK_BIN, path)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("runs the hwtest variant", p.stderr)


    def midi_fifo(self):
        path = os.path.join(self.tmp, "midi")
        os.mkfifo(path)
        return path

    def test_host_midi_matches(self):
        fifo = self.midi_fifo()
        session = os.path.join(self.tmp, "sessions")
        port = self.port([DEVHOST, "60", "--midi", fifo])
        p = self.device(port, DEVHOST, os.path.join(SCENARIOS, "note-off.jsonl"), "--midi", "raw:" + fifo,
                        "--session", session)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        line = next(x for x in p.stdout.splitlines() if "note-off.jsonl" in x)
        n = int(re.search(r"(\d+) MIDI messages, (\d+) received by the host", line).group(1))
        self.assertGreater(n, 0)
        self.assertIn("%d MIDI messages, %d received by the host" % (n, n), line)
        with open(os.path.join(session, "note-off", "midi.jsonl")) as f:
            rows = [json.loads(x) for x in f]
        self.assertEqual(len(rows), n)
        self.assertEqual((rows[0]["dir"], rows[0]["src"]), ("rx", "host"))
        self.assertIn("84 28 00", [r["hex"] for r in rows])

    def test_host_midi_loss_is_caught(self):
        fifo = self.midi_fifo()
        port = self.port([DEVHOST, "60", "--midi", fifo, "--fault", "drop-midi"])
        p = self.device(port, DEVHOST, os.path.join(SCENARIOS, "note-off.jsonl"), "--midi", "raw:" + fifo)
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("the host received other MIDI than the device logged, at message 2", p.stdout)
        self.assertNotIn("disagree", p.stdout)  # the core itself agrees with the simulator

    def test_bad_midi_source(self):
        p = self.device(self.port([DEVHOST, "30"]), DEVHOST, os.path.join(SCENARIOS, "note-off.jsonl"),
                        "--midi", "raw:" + os.path.join(self.tmp, "none"))
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("cannot open the MIDI source", p.stderr)
        p = self.device(self.port([DEVHOST, "30"]), DEVHOST, os.path.join(SCENARIOS, "note-off.jsonl"),
                        "--midi", "usb")
        self.assertIn("--midi wants raw:PATH or coremidi:NAME", p.stderr)


class MidiIn(unittest.TestCase):
    """midiin.py: the byte stream parser, the raw source, CoreMIDI's functions (not called)."""

    def setUp(self):
        sys.path.insert(0, HERE)
        import midiin
        self.m = midiin

    def test_parser(self):
        p = self.m.Parser()
        msgs = p.feed(bytes([0x90, 0x3C, 0x7F, 0x3E, 0x7F,  # running status
                             0xF8,                          # clock, anywhere
                             0xB0, 0x07, 0xF8, 0x64,        # even inside a message
                             0xF0, 0x7D, 0x01, 0xF7,        # sysex: left out
                             0x40, 0x40,                    # no running status after it
                             0xC2, 0x05, 0x06,              # program change, running
                             0xE1, 0x00]))
        self.assertEqual([self.m.hex_of(x) for x in msgs],
                         ["90 3C 7F", "90 3E 7F", "B0 07 64", "C2 05", "C2 06"])
        self.assertEqual([self.m.hex_of(x) for x in p.feed(bytes([0x40]))], ["E1 00 40"])  # split across reads

    def test_raw_source(self):
        tmp = tempfile.mkdtemp()
        path = os.path.join(tmp, "fifo")
        os.mkfifo(path)
        src = self.m.RawSource(path)
        try:
            fd = os.open(path, os.O_WRONLY)
            os.write(fd, bytes([0x90, 0x3C]))
            os.write(fd, bytes([0x7F, 0x80, 0x3C, 0x00]))
            os.close(fd)
            end = time.monotonic() + 2
            got = []
            while len(got) < 2 and time.monotonic() < end:
                got += src.take()
                time.sleep(0.01)
            self.assertEqual([self.m.hex_of(m) for _, m in got], ["90 3C 7F", "80 3C 00"])
            self.assertLessEqual(got[0][0], got[1][0])
            src.mark()
            self.assertEqual(src.take(), [])
        finally:
            src.close()

    def test_open_source(self):
        with self.assertRaises(self.m.MidiError):
            self.m.open_source("raw:")
        with self.assertRaises(self.m.MidiError):
            self.m.open_source("alsa:hw:1")

    @unittest.skipUnless(sys.platform == "darwin", "CoreMIDI is macOS's")
    def test_coremidi_functions_exist(self):
        cm, cf = self.m.coremidi_libs()  # loads the frameworks; talks to no MIDI server
        for fn in ("MIDIClientCreate", "MIDIInputPortCreate", "MIDIGetNumberOfSources", "MIDIGetSource",
                   "MIDIObjectGetStringProperty", "MIDIPortConnectSource", "MIDIPortDisconnectSource",
                   "MIDIPortDispose", "MIDIClientDispose"):
            self.assertTrue(hasattr(cm, fn), fn)
        for fn in ("CFStringCreateWithCString", "CFStringGetCString", "CFRelease"):
            self.assertTrue(hasattr(cf, fn), fn)

    def test_packet_list(self):
        # two packets as CoreMIDI lays them out (4-byte steps on Apple silicon)
        import platform
        align = 4 if platform.machine() == "arm64" else 1
        first = struct.pack("<QH", 7, 3) + bytes([0x90, 0x3C, 0x7F])
        first += b"\0" * ((-len(first)) % align)
        buf = ctypes.create_string_buffer(struct.pack("<I", 2) + first + struct.pack("<QH", 9, 2) + bytes([0xC0, 0x05]))
        self.assertEqual(self.m.packets(ctypes.addressof(buf)), [(7, bytes([0x90, 0x3C, 0x7F])), (9, bytes([0xC0, 0x05]))])


class DeviceAnalysis(unittest.TestCase):
    """device.analyse() on records made by the simulator with other timing than the scenario's."""

    def setUp(self):
        sys.path.insert(0, HERE)
        import device
        self.device = device
        self.tmp = tempfile.mkdtemp()
        self.path = scenario(self.tmp, [
            '{"turn": "enc1", "clicks": 3, "every_ms": 200}',
            '{"expect_midi": ["B0 66 41", "B0 66 42", "B0 66 43"]}',
        ])

    def records(self, every_ms):
        """What a device would log for the scenario's clicks every_ms apart."""
        log = os.path.join(self.tmp, "sim.bin")
        cmds = ["set config test", "level all 5"] + ["e %d 0 1 inj" % (100000 + k * every_ms * 1000) for k in range(3)]
        smcsim.run_sim(smcsim.DEFAULT_SIM, cmds, log)
        return [r for r in smcsim.decode_log(smcsim.DEFAULT_SIM, log) if r.get("kind") == "log"]

    def analyse(self, records):
        names = smcsim.sim_names(smcsim.DEFAULT_SIM)
        steps, tr, settings, per_step, ends = self.device.plan_scenario(self.path, names)
        injections = [([e], {"inject": "ok", "accepted": 1}) for e in per_step[0]]
        return self.device.analyse(self.path, steps, tr, settings, per_step, ends, records, injections,
                                   smcsim.DEFAULT_SIM)

    def test_on_time(self):
        failures, notes, d = self.analyse(self.records(200))
        self.assertEqual((failures, notes), ([], []))
        self.assertEqual((d["inputs"], d["messages"], d["late_ms"], d["agree"]), (3, 3, 0, True))
        self.assertNotIn("received", d)  # no MIDI source

    def test_host_midi(self):
        c = self.device.compare_host_midi
        dev = ["B0 66 41", "B0 66 42"]
        self.assertEqual(c("s", [(1, bytes([0xB0, 0x66, 0x41])), (2, bytes([0xB0, 0x66, 0x42]))], dev), [])
        f = c("s", [(1, bytes([0xB0, 0x66, 0x41]))], dev)
        self.assertEqual(f, ["s: the host received other MIDI than the device logged, at message 1: host nothing, "
                             "device B0 66 42 (1 received, 2 logged)"])
        f = c("s", [(1, bytes([0xB0, 0x66, 0x41])), (2, bytes([0xB0, 0x66, 0x40]))], dev)
        self.assertIn("at message 1: host B0 66 40, device B0 66 42", f[0])

    def test_timing_off_but_agreeing(self):
        # clicks 5 ms apart accelerate: the expectation fails, the simulator agrees on that timing
        failures, notes, d = self.analyse(self.records(5))
        self.assertEqual(len(failures), 1)
        self.assertIn("device: s.jsonl:2: MIDI", failures[0])
        self.assertIn("'B0 66 43', 'B0 66 46'", failures[0])
        self.assertTrue(d["agree"])
        self.assertEqual(d["late_ms"], 390)
        self.assertIn("expectations failed on the device's timing (inputs up to 390 ms off", notes[0])

    def test_buffered_input_output_keeps_its_cause_during_edit(self):
        # An edit's read window can include the preceding input's buffered output.
        # Its cause ID, not when the host read it, determines its scenario step.
        self.path = scenario(self.tmp, [
            '{"press": "r1"}',
            '{"expect_midi": ["90 28 7F"]}',
            '{"edit": {"preset": 1, "page": 1, "control": "fader1", "to": {}}}',
            '{"expect_midi": []}',
            '{"release": "r1"}',
            '{"expect_midi": ["90 28 00"]}',
        ])
        names = smcsim.sim_names(smcsim.DEFAULT_SIM)
        steps, tr, settings, per_step, ends = self.device.plan_scenario(self.path, names)
        cmds = smcsim.set_commands(settings) + ["level all 5"]
        injections = []
        for events in per_step:
            for e in events:
                if e[0] in smcsim.INPUTS:
                    cmds.append(smcsim.sim_command(e[:-1] + (True,)))
                    injections.append(([e], {"inject": "ok", "accepted": 1}))
                elif e[0] == "edit":
                    cmds.append(smcsim.sim_command(e))
        log = os.path.join(self.tmp, "sim.bin")
        smcsim.run_sim(smcsim.DEFAULT_SIM, cmds, log)
        recs = [r for r in smcsim.decode_log(smcsim.DEFAULT_SIM, log) if r.get("kind") == "log"]
        lo = next(i for i, r in enumerate(recs) if self.device.record_input(r))
        hi = next(i + 1 for i, r in enumerate(recs) if self.device.is_engine(r, "configuration edited"))
        failures, notes, d = self.device.analyse(
            self.path, steps, tr, settings, per_step, ends, recs, injections,
            smcsim.DEFAULT_SIM, edits=[(2, lo, hi)])
        self.assertEqual((failures, notes), ([], []))
        self.assertTrue(d["agree"])

    def test_actions_before_delayed_first_input_share_time_origin(self):
        from unittest.mock import patch

        # Selection and edit precede a first input delayed by 100 ms. Replaying
        # all three must preserve their intervals, not clamp the earlier actions.
        self.path = scenario(self.tmp, [
            '{"select": {"page": 2}}',
            '{"expect_state": {"page": 2}}',
            '{"edit": {"preset": 1, "page": 2, "control": "fader2", "to": {}}}',
            '{"fader": "fader1", "to": 64}',
            '{"expect_midi": ["B1 14 40"]}',
            '{"fader": "fader1", "to": 32}',
            '{"expect_midi": ["B1 14 20"]}',
        ])
        names = smcsim.sim_names(smcsim.DEFAULT_SIM)
        steps, tr, settings, per_step, ends = self.device.plan_scenario(self.path, names)
        actions = [(i, e) for i, events in enumerate(per_step) for e in events
                   if e[0] in smcsim.INPUTS or e[0] in ("edit", "select")]
        actual = [100000, 130000, 220000, 260000]
        cmds = smcsim.set_commands(settings) + ["level all 5"]
        injections = []
        for (_, e), t in zip(actions, actual):
            if e[0] in smcsim.INPUTS:
                injections.append(([e], {"inject": "ok", "accepted": 1}))
                e = e[:-1] + (True,)
            cmds.append(smcsim.sim_command(e, t))
        log = os.path.join(self.tmp, "sim.bin")
        smcsim.run_sim(smcsim.DEFAULT_SIM, cmds, log)
        recs = [r for r in smcsim.decode_log(smcsim.DEFAULT_SIM, log) if r.get("kind") == "log"]
        # The simulator calls core_select directly; only the control task logs
        # "select asked". Supply that command marker at its actual application time.
        lo = next(i for i, r in enumerate(recs) if r["msg"] == "preset 1 page 2")
        recs.insert(lo, dict(recs[lo], msg="select asked: preset 0 page 2 (0: keep)"))
        hi = next(i + 1 for i, r in enumerate(recs) if r["msg"] == "preset 1 page 2")
        edit_lo = next(i for i, r in enumerate(recs) if self.device.is_engine(r, "edit: preset"))
        edit_hi = next(i + 1 for i, r in enumerate(recs) if self.device.is_engine(r, "configuration edited"))
        before = json.dumps(recs, sort_keys=True)
        with patch.object(smcsim, "run_sim", wraps=smcsim.run_sim) as replay:
            failures, notes, d = self.device.analyse(
                self.path, steps, tr, settings, per_step, ends, recs, injections,
                smcsim.DEFAULT_SIM, edits=[(0, lo, hi), (2, edit_lo, edit_hi)])
        self.assertEqual((failures, notes), ([], []))
        self.assertTrue(d["agree"])
        # Delays remain visible relative to the first action, not the first input.
        base = actual[0] - actions[0][1][1]
        expected_late = max(abs(t - base - e[1]) for (_, e), t in zip(actions, actual)
                            if e[0] in smcsim.INPUTS) // 1000
        self.assertEqual(d["late_ms"], expected_late)
        commands = replay.call_args.args[1]
        replay_times = [int(c.split()[1]) for c in commands
                        if c.split()[0] in ("select", "edit", "f")]
        self.assertEqual(replay_times, [t - base for t in actual])
        self.assertEqual(json.dumps(recs, sort_keys=True), before)

    def test_missing_input(self):
        recs = self.records(200)
        drop = next(i for i, r in enumerate(recs) if r["msg"].startswith("encoder 1 +1"))
        failures, _, _ = self.analyse(recs[:drop] + recs[drop + 1:])
        self.assertEqual(len(failures), 1)
        self.assertIn("the device's input differs from the scenario's at input 2: device nothing", failures[0])

    def test_incomplete_log(self):
        failures, _, _ = self.analyse(self.records(200) + [{"kind": "lost", "count": 3}])
        self.assertIn("the device log is incomplete (lost); nothing compared", failures[0])


class CoreLog(unittest.TestCase):
    """corelog.py: the core's log records read back from their text."""

    def setUp(self):
        import corelog
        self.corelog = corelog

    def test_formats_are_in_the_core(self):
        """The wording the parsers depend on is what firmware/core/core.c logs."""
        with open(os.path.join(smcsim.ROOT, "firmware", "core", "core.c")) as f:
            source = f.read()
        missing = [fmt for fmt in self.corelog.FORMATS if '"%s"' % fmt not in source]
        self.assertEqual(missing, [], "core.c no longer logs these; change tools/smcsim/corelog.py with it")

    def test_input_records(self):
        p = self.corelog.parse_input
        self.assertEqual(p("button 44 down"), ("b", 44, 1, False))
        self.assertEqual(p("button 3 up (injected)"), ("b", 3, 0, True))
        self.assertEqual(p("encoder 1 +3"), ("e", 0, 3, False))
        self.assertEqual(p("encoder 8 -12 (injected)"), ("e", 7, -12, True))
        self.assertEqual(p("fader 2 = 16383"), ("f", 1, 16383, False))
        self.assertEqual(p("midi in 1: b0 14 40 (injected)"), ("m", 1, "b0 14 40", True))
        for other in ("button 3 sideways", "fader 2 = 5 or so", "button 3: 1 again", "bad event: type 9 index 0", ""):
            self.assertIsNone(p(other), other)

    def test_engine_records(self):
        c = self.corelog
        self.assertEqual(c.parse_out("out b0 14 40"), "B0 14 40")
        self.assertEqual(c.parse_out("out c2 05"), "C2 05")
        self.assertIsNone(c.parse_out("out b0"))
        self.assertEqual(c.parse_led("led 52 = 3"), (52, 3))
        self.assertEqual(c.parse_bank("preset 2 page 4"), (2, 4))
        self.assertIs(c.parse_shift("shift 1"), True)
        self.assertIs(c.parse_shift("shift 0"), False)
        self.assertIsNone(c.parse_shift("shift latched"))
        self.assertIsNone(c.parse_led("fader 1 picked up 81b00014 at 64"))

    def test_the_simulator_s_records_parse(self):
        """Every input the simulator is given comes back from its log, and its MIDI as "out" records."""
        log = os.path.join(tempfile.mkdtemp(), "sim.bin")
        cmds = ["set config test", "level all 5", "b 1000 44 1", "b 2000 40 1 inj", "b 3000 40 0", "b 4000 44 0",
                "e 5000 0 -2", "f 6000 3 8000 inj", "m 7000 ble b0 14 40", "b 8000 2 1", "b 9000 2 0"]
        lines = smcsim.run_sim(smcsim.DEFAULT_SIM, cmds, log)
        records = [r for r in smcsim.decode_log(smcsim.DEFAULT_SIM, log) if r.get("kind") == "log"]
        inputs = [self.corelog.parse_input(r["msg"]) for r in records if r["module"] == "input"]
        self.assertEqual(inputs, [("b", 44, 1, False), ("b", 40, 1, True), ("b", 40, 0, False), ("b", 44, 0, False),
                                  ("e", 0, -2, False), ("f", 3, 8000, True), ("m", 1, "b0 14 40", False),
                                  ("b", 2, 1, False), ("b", 2, 0, False)])
        engine = [r["msg"] for r in records if r["module"] == "engine"]
        outs = [o for o in map(self.corelog.parse_out, engine) if o]
        self.assertEqual(outs, [" ".join(line.split()[3:]) for line in lines if line.startswith("M ")])
        self.assertGreater(len(outs), 0)
        self.assertIn((1, 2), [b for b in map(self.corelog.parse_bank, engine) if b])  # Shift + ▼: page 2
        self.assertEqual([s for s in map(self.corelog.parse_shift, engine) if s is not None], [True, False])
        self.assertIn((44, 1), [x for x in map(self.corelog.parse_led, engine) if x])  # the Shift LED


if __name__ == "__main__":
    if not os.access(smcsim.DEFAULT_SIM, os.X_OK):
        sys.exit("test_smcsim: build the simulator first (make -C firmware sim)")
    unittest.main()

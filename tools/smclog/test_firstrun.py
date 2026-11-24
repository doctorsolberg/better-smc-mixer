"""Tests for firstrun.py. Run: python3 -I tools/smclog/test_firstrun.py

The whole first run against build/host/devhost (made by make -C firmware test) on a pseudo-terminal
with a keep file: RESET restarts devhost in place, as a reset restarts the device, so the soft,
fault and watchdog tests all run. Also: no device on the port, another build on it, and the debug
resets refused without injection. Skipped without devhost.
"""
import json
import os
import subprocess
import sys
import tempfile
import tty
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import smclog as sl  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
DEVHOST = os.path.join(ROOT, "build", "host", "devhost")
FIRSTRUN = os.path.join(HERE, "firstrun.py")
SMCLOG = os.path.join(HERE, "smclog.py")


class StandIn:
    """devhost on a pseudo-terminal. The test keeps the terminal's side open, as the device stays
    while the tool closes and reopens its port."""

    def __init__(self, tmp, *args):
        self.master, self.slave = os.openpty()
        tty.setraw(self.slave)
        self.port = os.ttyname(self.slave)
        self.proc = subprocess.Popen([DEVHOST, "60", "--keep", os.path.join(tmp, "keep.bin")] + list(args),
                                     stdin=self.master, stdout=self.master)

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()
        os.close(self.master)
        os.close(self.slave)


def firstrun(*args):
    return subprocess.run([sys.executable, "-I", FIRSTRUN, "--quiet"] + list(args), capture_output=True, text=True,
                          timeout=120)


def items(report, step, status=None):
    return [i for i in report["items"] if i["step"] == step and (status is None or i["status"] == status)]


@unittest.skipUnless(os.access(DEVHOST, os.X_OK), "build/host/devhost not built (make -C firmware test)")
class FirstRun(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.session = os.path.join(self.tmp.name, "session")

    def tearDown(self):
        self.tmp.cleanup()

    def report(self):
        with open(os.path.join(self.session, "report.json")) as f:
            return json.load(f)

    def test_the_whole_run_on_the_stand_in(self):
        dev = StandIn(self.tmp.name)
        try:
            p = firstrun("--port", dev.port, "--binary", DEVHOST, "--session", self.session, "--watchdog",
                         "--wait", "5", "--listen", "6")
        finally:
            dev.close()
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        rep = self.report()
        self.assertEqual(rep["failed"], 0)
        self.assertEqual(rep["meta"]["resets"], ["soft", "fault", "watchdog"])
        self.assertEqual(rep["info"]["caps"], ["log", "inject", "metrics", "setup", "boot_log", "reset",
                                                   "config", "events", "sent"])
        with open(os.path.join(ROOT, "VERSION"), encoding="ascii") as f:
            self.assertEqual(rep["info"]["fw_version"], f.read().strip())  # devhost reports the product's
        texts = {s: [i["text"] for i in items(rep, s, "pass")] for s in
                 ("link", "ping", "stream", "boot", "levels", "inject", "metrics", "reset soft", "reset fault",
                  "reset watchdog", "clock")}
        self.assertIn("20 of 20 PINGs answered", texts["ping"])
        self.assertTrue(texts["stream"][0].startswith("the stream starts with its header"))
        self.assertTrue(texts["boot"][0].startswith("boot 1: journal done"))
        self.assertEqual(len(texts["levels"]), 1)
        self.assertIn("button 0 pressed and released: ok, 2 accepted, 0 refused", texts["inject"])
        self.assertTrue(texts["metrics"][0].endswith("its checks hold"))
        for how, boot, cause in (("soft", 2, "0x40 (soft reset"), ("fault", 3, "0x40 (soft reset"),
                                 ("watchdog", 4, "0x4 (watchdog")):
            t = texts["reset " + how]
            self.assertTrue(t[0].startswith("the device reset and came back"), t)
            self.assertIn("the RAM was kept across the reset: boot %d after boot %d" % (boot, boot - 1), t)
            self.assertTrue(any(x.startswith("reset cause " + cause) for x in t), t)
            self.assertTrue(any(x.startswith("the previous run's last heartbeat") for x in t), t)
            self.assertTrue(any(x.endswith("records of the previous run replayed, up to the reset request") for x in t), t)
        self.assertTrue(any(x.startswith("the exception handler wrote its crash record: signal (devhost), task devhost")
                            for x in texts["reset fault"]))
        self.assertIn("no crash record, as expected", texts["reset soft"])
        self.assertIn("no crash record, as expected", texts["reset watchdog"])
        self.assertEqual([i["text"] for i in items(rep, "reset fault", "skip")],
                         ["the frame's order: devhost writes a stand-in frame, not the BR25 handler's"
                          if sys.platform.startswith("linux")  # devhost is an ELF there
                          else "the frame's order: no ELF (--binary) to name its addresses"])  # a Mach-O
        self.assertTrue(texts["clock"][0].startswith("run 1: the device's clock"))
        self.assertTrue(items(rep, "stream", "pass")[-1]["text"].endswith("0 bad frames, 0 gaps, 0 unknown call sites"))
        with open(os.path.join(self.session, "device.jsonl")) as f:
            recs = [json.loads(x) for x in f]
        self.assertEqual(sorted({r["conn"] for r in recs}), [1, 2, 3, 4])  # a connection per boot
        self.assertGreater(os.path.getsize(os.path.join(self.session, "device.bin")), 1000)
        with open(os.path.join(self.session, "report.txt")) as f:
            text = f.read()
        self.assertIn(", 0 failed, ", text)
        self.assertIn("--- reset fault: the boot journal", text)
        self.assertIn("CRASH in boot 2 at about", text)
        p = subprocess.run([sys.executable, "-I", SMCLOG, "boot", self.session], capture_output=True, text=True,
                           timeout=20)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("boot 4 since the RAM was lost: reset cause 0x4 (host restart)", p.stdout)

    def test_no_device(self):
        master, slave = os.openpty()  # a port with nothing behind it
        try:
            p = firstrun("--port", os.ttyname(slave), "--binary", DEVHOST, "--session", self.session, "--wait", "0.5")
        finally:
            os.close(master)
            os.close(slave)
        self.assertEqual(p.returncode, 1)
        rep = self.report()
        self.assertEqual([(i["step"], i["status"]) for i in rep["items"]][:1], [("link", "fail")])
        self.assertTrue(rep["items"][0]["text"].startswith("no answer to GET_INFO"))
        self.assertIsNone(rep["info"])

    def test_another_build(self):
        d = sl.make_dictionary(DEVHOST)
        d["build_id"] = "00" * 16
        path = os.path.join(self.tmp.name, "other.json")
        with open(path, "w") as f:
            json.dump(d, f)
        dev = StandIn(self.tmp.name)
        try:
            p = firstrun("--port", dev.port, "--dict", path, "--session", self.session, "--wait", "3")
        finally:
            dev.close()
        self.assertEqual(p.returncode, 1)
        fails = [i["text"] for i in self.report()["items"] if i["status"] == "fail"]
        self.assertEqual(len(fails), 1)
        self.assertTrue(fails[0].startswith("the device runs build"), fails)

    def test_debug_resets_need_injection(self):
        dev = StandIn(self.tmp.name)
        try:
            cmd = [sys.executable, "-I", SMCLOG, "cmd", "--port", dev.port]
            p = subprocess.run(cmd + ["reset", "fault"], capture_output=True, text=True, timeout=20)
            self.assertEqual(p.returncode, 1)
            self.assertEqual(p.stdout.strip(), "reset: disabled (inject on first)")
            p = subprocess.run(cmd + ["--json", "reset", "watchdog"], capture_output=True, text=True, timeout=20)
            self.assertEqual(json.loads(p.stdout)["reset"], "disabled")
            p = subprocess.run(cmd + ["info"], capture_output=True, text=True, timeout=20)
            self.assertIn("capabilities log,inject,metrics,setup,boot_log,reset", p.stdout)
            self.assertIsNone(dev.proc.poll())  # still the same process: nothing restarted
        finally:
            dev.close()


APP_ELF = os.path.join(ROOT, "build", "app", "app.elf")


@unittest.skipUnless(os.path.exists(APP_ELF), "build/app/app.elf missing (tools/env/build_app.sh)")
class FrameCheck(unittest.TestCase):
    """The fault test's check of the frame's order, on the image's own symbols."""

    def check(self, regs, trace=()):
        import firstrun
        funcs = sl.elf_functions(APP_ELF)
        rep = firstrun.Report(quiet=True)
        crash = {"build_tag": "t", "regs": regs, "trace": list(trace), "stack": []}
        firstrun.check_frame(rep, "reset fault", {"build_tag": "t", "crash": crash}, funcs, {})
        return rep.items

    def test_inside_the_fault_function(self):
        names = {f[2]: f for f in sl.elf_functions(APP_ELF)}
        self.assertIn(firstrun_fault(), names)  # a function of its own in the image, not inlined
        start = names[firstrun_fault()][0]
        out = self.check({"RETI": 0x1E00000, "RETX": start + 6, "RETS": names["smclog_write"][0] + 4}, [start + 6])
        self.assertEqual(out[0]["status"], "pass")
        self.assertTrue(out[0]["text"].startswith("the frame's order holds: RETX inside diag_br25_fault"), out[0])
        self.assertIn("RETS smclog_write+0x4", out[0]["text"])
        self.assertEqual(out[1]["text"], "the CPU's trace: diag_br25_fault+0x6")

    def test_taken_after_the_return(self):
        """The load traps once the function has returned: the frame names the caller, the trace
        shows the function just before it."""
        names = {f[2]: f for f in sl.elf_functions(APP_ELF)}
        start = names[firstrun_fault()][0]
        after = names["smclog_write"][0] + 4
        out = self.check({"RETI": after, "RETS": after}, [after, start + 6, after - 4])
        self.assertEqual(out[0]["status"], "pass", out[0])
        self.assertTrue(out[0]["text"].startswith("the frame's order holds: RETI smclog_write+0x4 is where"), out[0])
        out = self.check({"RETI": after}, [after, names["smclog_write"][0]])  # the function not in the trace
        self.assertEqual(out[0]["status"], "fail")

    def test_elsewhere(self):
        names = {f[2]: f for f in sl.elf_functions(APP_ELF)}
        out = self.check({"RETI": names["smclog_write"][0], "RETE": 0x5A5A5A5A})
        self.assertEqual(out[0]["status"], "fail")
        self.assertIn("return addresses: RETI smclog_write", out[0]["text"])
        out = self.check({})
        self.assertEqual(out[0]["status"], "fail")

    def test_devhost_frame_skipped(self):
        """devhost's crash record is a stand-in frame (its ELF on Linux gives functions): skipped."""
        import firstrun
        rep = firstrun.Report(quiet=True)
        crash = {"build_tag": "t", "regs": {"RETI": 0xc0de0010}, "trace": [], "stack": [],
                 "cause_name": sl.CRASH_CAUSES[9]}
        firstrun.check_frame(rep, "reset fault", {"build_tag": "t", "crash": crash}, [(0x100, 4, "f")], {})
        self.assertEqual(rep.items[0]["status"], "skip", rep.items)


def firstrun_fault():
    import firstrun
    return firstrun.FAULT_FUNCTION


if __name__ == "__main__":
    unittest.main()

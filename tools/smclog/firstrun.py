"""firstrun: the first run of our firmware on the device in one command (docs/hwtest.md, "The first
run"). It drives the device over the command link (docs/link.md), records a session (LOG-21) and
writes a report of what passed, what failed and what was only seen.

Usage: python3 -I tools/smclog/firstrun.py --port DEV (--binary ELF | --dict DICT.json)
           --session DIR [--resets soft,fault] [--watchdog] [--no-reset] [--wait SECONDS]
           [--listen SECONDS]

Steps and their checks:
  link      GET_INFO: the link and log format versions; the build ID against the dictionary's
  ping      20 PINGs: all answered, the round trips, the device's clock going forward
  stream    LOG_SUBSCRIBE: the stream's header first
  boot      BOOT_LOG: this boot's journal (reset cause, previous run, crash record, init times)
  levels    LOG_GET_LEVELS, LOG_SET_LEVEL: a level changed and put back
  raw       LOG_RAW_INPUT on and off, if offered: records in a second each way
  inject    INPUT_INJECT on, a button pressed and released, off, if offered
  metrics   METRICS_NOW: a heartbeat, summed up and checked as `smclog metrics --check` does
  listen    --listen seconds at rest (default 10), with a PING every second for the clock
  reset     RESET for each mode asked (default soft, then fault): the device resets (its clock
            starts again) and comes back; its next
            boot's journal counts on (the RAM was kept across the reset), names the cause, reports
            the heartbeat sent just before and replays the latest records up to the request. After
            the fault test: the crash record (our exception handler ran) and, with the ELF, a
            return address of the frame inside diag_br25_fault (the frame's order holds).
  stream    the whole session: no bad frame, no gap, no unknown call site; lost records counted
  clock     the device's clock against the host's (`smclog clock`): drift within 500 ppm

The session directory: meta.json; device.bin, the raw bytes of every connection in order;
device.jsonl, the decoded records, each with "conn" (a new connection after each reset);
report.json and report.txt. The exit status is 1 if a check failed.

Nothing here writes flash (C-6). The fault and watchdog tests are debug functions (LOG-34): the
tool switches injection on first. The watchdog test hangs the device until its watchdog (8 s)
resets it; if the watchdog were off, the device would stay hung until its power is removed, so
that test runs only with --watchdog. Standard library only.
"""
import argparse
import json
import os
import platform
import struct
import sys
import termios
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import smclog as S  # noqa: E402

FAULT_FUNCTION = "diag_br25_fault"  # firmware/platform/br25/diag_br25.c
RETURN_REGS = ("RETI", "RETE", "RETX", "RETS")
EXPECTED_CAUSE = {"soft": "soft reset", "fault": "soft reset", "watchdog": "watchdog"}
DRIFT_LIMIT_PPM = 500
CLOCK_SPAN_MIN_S = 5.0
SYNC_S = 1.0  # a PING for the clock while listening
RESET_MARGIN_US = 50000  # the device resets 100 ms after its reply: its clock must lag at least that
PINGS = 20


class Report:
    def __init__(self, quiet=False):
        self.items = []
        self.quiet = quiet

    def add(self, step, status, text, **data):
        self.items.append({"step": step, "status": status, "text": text, "data": data})
        if not self.quiet:
            print("%-4s  %-14s %s" % (status.upper(), step, text), flush=True)

    def failed(self):
        return [i for i in self.items if i["status"] == "fail"]

    def text(self, head):
        lines = list(head) + [""]
        lines += ["%-4s  %-14s %s" % (i["status"].upper(), i["step"], i["text"]) for i in self.items]
        counts = {s: sum(1 for i in self.items if i["status"] == s) for s in ("pass", "fail", "info", "skip")}
        lines += ["", "%(pass)d passed, %(fail)d failed, %(info)d seen, %(skip)d skipped" % counts]
        for i in self.items:
            if i["data"].get("journal"):
                lines += ["", "--- %s: the boot journal" % i["step"], i["data"]["journal"]]
            if i["data"].get("metrics"):
                lines += ["", "--- %s" % i["step"], i["data"]["metrics"]]
        return "\n".join(lines) + "\n"


class Session:
    """The session directory: every connection's raw bytes and decoded records."""

    def __init__(self, path, dictionary):
        os.makedirs(path, exist_ok=True)
        self.path = path
        self.dictionary = dictionary
        self.raw = open(os.path.join(path, "device.bin"), "wb")
        self.jsonl = open(os.path.join(path, "device.jsonl"), "w")
        self.records = []
        self.conn = 0

    def record(self, r):
        r["conn"] = self.conn
        self.records.append(r)
        self.jsonl.write(json.dumps(r, sort_keys=True) + "\n")

    def close(self):
        self.raw.close()
        self.jsonl.close()


class Device(S.Link):
    """One connection: the command link on the open port, its bytes and records into the session."""

    def __init__(self, fd, session):
        super().__init__(fd, S.Decoder(session.dictionary), on_record=session.record)
        self.session = session
        self.conn = session.conn
        self.next_sync = time.monotonic() + SYNC_S

    def read(self, timeout):
        recs, data = super().read(timeout)
        if data:
            self.session.raw.write(data)
        return recs, data

    def call(self, cmd, payload=b"", timeout=1.0, tries=3):
        r = super().call(S.CMDS[cmd], payload, timeout, tries)
        self.session.record(r)
        return r

    def records(self):
        return [r for r in self.session.records if r["conn"] == self.conn]

    def listen(self, seconds, until=None, since=None):
        """Records for up to seconds, a PING every second for the clock; True as soon as until(record)
        holds for one that arrives, or for one of this connection's records from index since on."""
        if until and since is not None and any(until(r) for r in self.records()[since:]):
            return True
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if time.monotonic() >= self.next_sync:
                self.send(S.CMDS["ping"], struct.pack("<Q", time.monotonic_ns()))
                self.next_sync += SYNC_S
            recs, _ = self.read(min(0.1, max(0.0, deadline - time.monotonic())))
            hit = False
            for r in recs:
                self.session.record(r)
                hit = hit or bool(until and until(r))
            if hit:
                return True
        return False

    def close(self):
        os.close(self.fd)


def connect(port, session, wait):
    """The device answering GET_INFO on the port within wait seconds: (Device, info) or (None, None).
    Bytes waiting from before (a previous connection) are dropped."""
    deadline = time.monotonic() + wait
    session.conn += 1
    while True:
        fd = None
        try:
            fd = S.open_serial(port)
            if os.isatty(fd):
                termios.tcflush(fd, termios.TCIFLUSH)
            dev = Device(fd, session)
            return dev, dev.call("info", timeout=0.3, tries=1)
        except (OSError, S.SmclogError):
            if fd is not None:
                os.close(fd)
        if time.monotonic() >= deadline:
            return None, None
        time.sleep(0.2)


def is_boot_end(r):
    return r.get("module") == "boot" and r.get("msg", "").startswith(("boot done", "boot not done yet"))


def is_heartbeat_end(r):
    return r.get("module") == "metrics" and r.get("msg", "").startswith("log: ")


def last_ping(dev):
    """(device µs, host ns at its arrival) of this connection's last PING reply, or None."""
    pings = [(r["t_us"], r["t_host_ns"]) for r in dev.records() if r.get("kind") == "reply" and
             r.get("cmd") == "ping" and "t_us" in r and "t_host_ns" in r]
    return pings[-1] if pings else None


# --- the steps -----------------------------------------------------------------------------------

def step_link(rep, info, dictionary):
    ok = info["link_version"] == S.LINK_VERSION and info["log_format"] == S.FORMAT_VERSION
    rep.add("link", "pass" if ok else "fail", "link version %d, log format %d (this tool: %d, %d)" % (
        info["link_version"], info["log_format"], S.LINK_VERSION, S.FORMAT_VERSION))
    rep.add("link", "info", "firmware %s, variant %s, capabilities %s, log ring %d bytes, timer %d Hz" % (
        info.get("fw_version") or "(no version)", info["variant"], ",".join(info["caps"]), info["ring_size"],
        info["timer_hz"]), info=info)
    want = dictionary.get("build_id")
    if want and want != info["build_id"]:
        rep.add("link", "fail", "the device runs build %s, the dictionary is for %s: give that build's ELF"
                % (info["build_id"], want))
        return False
    rep.add("link", "pass", "build %s, the dictionary's" % info["build_id"])
    return True


def step_ping(rep, dev):
    rtts, times = [], []
    for _ in range(PINGS):
        t0 = time.monotonic_ns()
        try:
            r = dev.call("ping", struct.pack("<Q", t0), timeout=0.5)
        except S.SmclogError:
            continue
        rtts.append((time.monotonic_ns() - t0) / 1e3)
        times.append(r["t_us"])
    rep.add("ping", "pass" if len(rtts) == PINGS else "fail", "%d of %d PINGs answered" % (len(rtts), PINGS))
    if len(times) > 1:
        forward = all(b >= a for a, b in zip(times, times[1:]))
        rep.add("ping", "pass" if forward else "fail", "the device's clock %s (%.6f s to %.6f s)" % (
            "went forward" if forward else "went back", times[0] / 1e6, times[-1] / 1e6))
    if rtts:
        rtts.sort()
        rep.add("ping", "info", "round trip min %.0f, median %.0f, max %.0f us" % (
            rtts[0], rtts[len(rtts) // 2], rtts[-1]), rtt_us=rtts)


def step_stream(rep, dev):
    r = dev.call("subscribe")
    if r["status"] != "ok":
        rep.add("stream", "fail", "LOG_SUBSCRIBE: %s" % r["status"])
        return False
    dev.listen(1.0, until=lambda x: x["kind"] == "header")
    first = next((x for x in dev.records() if x["kind"] != "reply"), None)
    if first is None or first["kind"] != "header":
        rep.add("stream", "fail", "the stream does not start with its header: %s" % (first and first["kind"]))
        return False
    rep.add("stream", "pass", "the stream starts with its header: format %d, timer %d Hz, %d records lost before "
            "it, ring %d bytes" % (first["version"], first["timer_hz"], first["lost_total"], first["ring_size"]))
    dev.listen(2.0, until=is_boot_end, since=0)  # the boot's records, still in the ring
    return True


def journal_now(dev):
    """BOOT_LOG and its journal: (the journal of this boot as it streamed after the reset, with the
    records it replayed, or None; the journal again, or None)."""
    mark = len(dev.records())
    r = dev.call("boot")
    if r["status"] != "ok":
        return None, None
    dev.listen(3.0, until=is_boot_end, since=mark)
    journals = S.read_boot(dev.records())
    first = next((j for j in journals if not j["again"]), None)
    again = next((j for j in reversed(journals) if j["again"]), None)
    return first, again


def describe_cause(j):
    return "0x%x (%s; the device's names: %s)" % (j["reset"]["raw"], ", ".join(j["reset"]["bits"]) or "no bit",
                                                   j["reset"]["text"])


def step_boot(rep, dev, funcs):
    first, again = journal_now(dev)
    j = first or again  # the first has the replayed records with it
    if j is None:
        rep.add("boot", "fail", "no boot journal after BOOT_LOG")
        return None
    done = j.get("done_us")
    rep.add("boot", "pass" if done is not None else "fail", "boot %d: journal %s" % (
        j["boot"], "done %d us after it started" % done if done is not None else "not done"),
        journal=S.boot_text(j, funcs))
    rep.add("boot", "info", "reset cause %s; firmware %s, build tag %s" % (describe_cause(j), j["version"],
                                                                           j["build_tag"]))
    prev = j.get("previous", "?")
    rep.add("boot", "info", "previous run: %s" % (
        "nothing kept" if prev is None else
        "ended before its first heartbeat" if isinstance(prev, dict) and not prev["heartbeat"] else
        "last heartbeat %d, %d ms after it started" % (prev["heartbeat"], prev["up_ms"]) if isinstance(prev, dict)
        else "not reported"))
    if j.get("crash"):
        c = j["crash"]
        rep.add("boot", "info", "a crash record from before: boot %d, %s, task %s" % (
            c["boot"], c["cause_name"], c["task"]))
    if j["marks"]:
        rep.add("boot", "info", "init: " + ", ".join("%s %d us" % (m["what"], m["took_us"]) for m in j["marks"]))
    return j


def step_levels(rep, dev, dictionary):
    levels = dev.call("levels")["levels"]
    old = levels[0]
    new = S.level_value("debug") if old != S.level_value("debug") else S.level_value("info")
    dev.call("level", bytes([0, new]))
    changed = dev.call("levels")["levels"][0] == new
    dev.call("level", bytes([0, old]))
    back = dev.call("levels")["levels"][0] == old
    rep.add("levels", "pass" if changed and back else "fail", "module 0's level %s to %d and back to %d" % (
        "set" if changed and back else "NOT set", new, old))
    names = {s["module_index"]: s["module"] for s in dictionary.get("sites", {}).values() if "module_index" in s}
    rep.add("levels", "info", " ".join("%s=%s" % (names.get(i, str(i)), S.LEVELS.get(v, "OFF" if v == 0 else v))
                                       for i, v in enumerate(levels)))


def step_raw(rep, dev, info):
    counts = {}
    for on in (3, 0):  # the matrix and the faders, then nothing
        r = dev.call("raw", bytes([on]))
        if r["status"] != "ok":
            rep.add("raw", "fail", "LOG_RAW_INPUT %d: %s" % (on, r["status"]))
            return
        mark = len(dev.records())
        dev.listen(1.0)
        counts[on] = sum(1 for x in dev.records()[mark:] if x["kind"] == "log")
    if info["variant"] == "hwtest":
        dev.call("raw", b"\x01")  # its default: the matrix only
    rep.add("raw", "pass", "raw input switched on and off")
    rep.add("raw", "info", "log records in a second: %d with raw input on, %d off (at rest)" % (counts[3], counts[0]))


def step_inject(rep, dev):
    r = dev.call("inject", S.inject_payload(["on"]))
    if r.get("inject") != "ok":
        rep.add("inject", "fail", "injection on: %s" % r.get("inject", r["status"]))
        return
    mark = len(dev.records())
    r = dev.call("inject", S.inject_payload(["button:0:press", "button:0:release"]))
    ok = r.get("inject") == "ok" and r["accepted"] == 2 and r["refused"] == 0
    rep.add("inject", "pass" if ok else "fail", "button 0 pressed and released: %s, %d accepted, %d refused" % (
        r.get("inject", r["status"]), r.get("accepted", 0), r.get("refused", 0)))
    dev.listen(0.5)
    seen = sum(1 for x in dev.records()[mark:] if "injected" in x.get("msg", ""))
    r = dev.call("inject", S.inject_payload(["off"]))
    rep.add("inject", "pass" if r.get("inject") == "ok" else "fail", "injection off")
    rep.add("inject", "info", "%d log records name injected input" % seen)


def heartbeat(dev):
    """METRICS_NOW and the heartbeat it writes: its records, its number (or None)."""
    mark = len(dev.records())
    if dev.call("metrics")["status"] != "ok":
        return [], None
    dev.listen(3.0, until=is_heartbeat_end, since=mark)
    dev.listen(0.3)  # the heap and stack records after it
    recs = dev.records()[mark:]
    beats = S.read_metrics(recs)
    return beats, (beats[-1]["seq"] if beats else None)


def step_metrics(rep, dev):
    beats, _ = heartbeat(dev)
    if not beats:
        rep.add("metrics", "fail", "no heartbeat after METRICS_NOW")
        return
    s = S.summarize_metrics(beats)
    rep.add("metrics", "pass" if not s["failed"] else "fail", "heartbeat %d%s" % (
        beats[-1]["seq"], ": " + "; ".join(s["failed"]) if s["failed"] else ", its checks hold"),
        metrics=S.metrics_text(s))


def step_reset(rep, a, session, dev, info, how, before, funcs):
    """RESET in one mode and the next boot: (the new connection or None, its journal or before)."""
    step = "reset " + how
    _, beat = heartbeat(dev) if "metrics" in info["caps"] else ([], None)
    if how != "soft":
        r = dev.call("inject", S.inject_payload(["on"]))
        if r.get("inject") != "ok":
            rep.add(step, "fail", "injection on (for a debug reset): %s" % r.get("inject", r["status"]))
            return dev, before
    dev.call("ping", struct.pack("<Q", time.monotonic_ns()))
    before_ping = last_ping(dev)
    r = dev.call("reset", bytes([S.RESET_HOW[how]]))
    if r["status"] != "ok" or r.get("reset") != "ok":
        rep.add(step, "fail", "RESET %s refused: %s" % (how, r.get("reset", r["status"])))
        return dev, before
    try:
        dev.listen(0.3)  # the last records before the reset
    except (OSError, S.SmclogError):
        pass
    dev.close()
    time.sleep(1.0 if how == "watchdog" else 0.5)
    wait = a.wait + (10.0 if how == "watchdog" else 0.0)
    new, info2 = connect(a.port, session, wait)
    if new is None:
        rep.add(step, "fail", "the device did not come back within %g s" % wait)
        return None, before
    if info2["build_id"] != info["build_id"]:
        rep.add(step, "fail", "the device came back with build %s" % info2["build_id"])
        return new, before
    new.call("ping", struct.pack("<Q", time.monotonic_ns()))
    t_after, h_after = last_ping(new)
    t_before, h_before = before_ping
    went_on = (h_after - h_before) / 1e3  # µs on the host since the PING before the reset
    if t_after >= t_before + went_on - RESET_MARGIN_US:  # its clock ran on: no reset
        note = ""
        if any("did not trap" in x.get("msg", "") for x in session.records):
            note = ": the fault test's load did not trap"
        rep.add(step, "fail", "the device did not reset: its clock went on from %.3f s to %.3f s in %.3f s%s" % (
            t_before / 1e6, t_after / 1e6, went_on / 1e6, note))
        return new, before
    rep.add(step, "pass", "the device reset and came back: up %.3f s, %.3f s after the request" % (
        t_after / 1e6, went_on / 1e6))
    new.call("subscribe")
    new.listen(2.0, until=is_boot_end, since=0)  # the new boot's journal and replayed records, from the ring
    first, again = journal_now(new)
    j = first or again  # the first has the replayed records with it
    if j is None:
        rep.add(step, "fail", "no boot journal after the reset")
        return new, before
    rep.add(step, "info", "the journal", journal=S.boot_text(j, funcs))

    kept = before is not None and j["boot"] == before["boot"] + 1
    if kept:
        rep.add(step, "pass", "the RAM was kept across the reset: boot %d after boot %d" % (j["boot"], before["boot"]))
    elif j.get("previous") is None:
        rep.add(step, "fail", "nothing kept: boot %d, the block a reset keeps reads as lost (does the mask ROM or "
                "the uboot clear that RAM?)" % j["boot"])
    else:
        rep.add(step, "info", "boot %d after boot %s" % (j["boot"], before and before["boot"]))

    want = EXPECTED_CAUSE[how]
    rep.add(step, "pass" if want in j["reset"]["bits"] else "info", "reset cause %s%s" % (
        describe_cause(j), "" if want in j["reset"]["bits"] else "; expected %s: note what the bits say" % want))

    prev = j.get("previous")
    if beat is not None and kept:
        ok = isinstance(prev, dict) and prev.get("heartbeat", 0) >= beat
        rep.add(step, "pass" if ok else "fail", "the previous run's last heartbeat: %s (sent %d before the reset)"
                % (prev.get("heartbeat") if isinstance(prev, dict) else "none", beat))

    if first is None:
        rep.add(step, "info", "the new boot's own journal was not in the stream (lost before the subscription?): "
                "the replay is not checked")
    elif isinstance(first.get("recent"), int) and first["recent"] > 0:
        tail = [x.get("msg", "") for x in first["past"][-8:]]
        asked = any(m.startswith("reset on request") for m in tail)
        rep.add(step, "pass" if asked else "info", "%d records of the previous run replayed%s" % (
            first["recent"], ", up to the reset request" if asked else "; the reset request is not among the last"))
    elif kept:
        rep.add(step, "fail", "the previous run's records were not replayed (%s)" % first.get("recent"))

    c = j.get("crash")
    if how == "fault":
        if not c:
            rep.add(step, "fail", "no crash record after the fault test: the exception handler did not run%s" % (
                "" if kept else ", or the RAM was not kept"))
        else:
            rep.add(step, "pass", "the exception handler wrote its crash record: %s, task %s, EMU_MSG 0x%08x" % (
                c["cause_name"], c["task"], c["regs"].get("EMU_MSG", 0)))
            check_frame(rep, step, j, funcs, info)
    elif c:
        rep.add(step, "info", "a crash record after a %s: %s, task %s" % (how, c["cause_name"], c["task"]))
    else:
        rep.add(step, "pass", "no crash record, as expected")
    return new, j


def check_frame(rep, step, j, funcs, info):
    """A return address of the crash frame inside the fault test's function: the frame's order holds.
    The misaligned load may trap late, once the function has returned (seen on the mixer,
    2026-11-11): then a return address equal to the trace's newest entry, with the fault test's
    function just before it in the trace, holds the frame's order too."""
    c = j["crash"]
    if not funcs:
        rep.add(step, "skip", "the frame's order: no ELF (--binary) to name its addresses")
        return
    if c["build_tag"] != j["build_tag"]:
        rep.add(step, "skip", "the frame's order: the crash record is from build tag %s" % c["build_tag"])
        return
    if c.get("cause_name") == S.CRASH_CAUSES[9]:  # devhost's stand-in frame (0xc0de...), no BR25 handler:
        # only its ELF on Linux gets here (macOS's Mach-O gives no functions)
        rep.add(step, "skip", "the frame's order: devhost writes a stand-in frame, not the BR25 handler's")
        return
    syms = S.boot_symbols(j, funcs)
    hits = sorted(k for k, v in syms.items() if v.split("+")[0] == FAULT_FUNCTION)
    named = ", ".join("%s %s" % (k, syms[k]) for k in RETURN_REGS if k in syms) or "none named"
    trace = c.get("trace", [])
    late = [k for k in RETURN_REGS if len(trace) >= 2 and c["regs"].get(k) == trace[0]
            and syms.get("trace1", "").split("+")[0] == FAULT_FUNCTION]
    if any(k in RETURN_REGS for k in hits):
        rep.add(step, "pass", "the frame's order holds: %s inside %s (return addresses: %s)" % (
            ", ".join(k for k in hits if k in RETURN_REGS), FAULT_FUNCTION, named), symbols=syms)
    elif late:
        rep.add(step, "pass", "the frame's order holds: %s %s is where the exception was taken, the trace's "
                "newest entry, just after %s returned (the load trapped late); return addresses: %s" % (
                    late[0], syms[late[0]], FAULT_FUNCTION, named), symbols=syms)
    else:
        rep.add(step, "fail", "no return address of the frame inside %s: the frame's order (diag.h) or the "
                "handler's push is wrong; return addresses: %s; elsewhere: %s" % (
                    FAULT_FUNCTION, named, ", ".join(hits) or "nothing"), symbols=syms)
    trace = ", ".join("%s" % syms.get("trace%d" % i, "0x%08x" % v) for i, v in enumerate(c.get("trace", [])))
    rep.add(step, "info", "the CPU's trace: %s" % (trace or "empty"))


def step_integrity(rep, records):
    kinds = {}
    for r in records:
        kinds[r["kind"]] = kinds.get(r["kind"], 0) + 1
    unknown = sum(1 for r in records if r.get("error"))
    lost = sum(r["count"] for r in records if r["kind"] == "lost")
    bad = kinds.get("bad_frame", 0)
    gaps = kinds.get("gap", 0)
    rep.add("stream", "pass" if not (bad or gaps or unknown) else "fail",
            "the whole session: %d records, %d bad frames, %d gaps, %d unknown call sites" % (
                len(records), bad, gaps, unknown))
    rep.add("stream", "info", "%d log records lost on the device (LOST)" % lost)


def step_clock(rep, records):
    fits = S.clock_fit(S.clock_samples(records))
    if not fits:
        rep.add("clock", "info", "no PING samples")
    for i, f in enumerate(fits, 1):
        if f["drift_ppm"] is None or f["span_s"] < CLOCK_SPAN_MIN_S:
            rep.add("clock", "info", "run %d: %.1f s of PINGs, too short for the drift" % (i, f["span_s"]))
            continue
        ok = abs(f["drift_ppm"]) <= DRIFT_LIMIT_PPM
        rep.add("clock", "pass" if ok else "fail", "run %d: the device's clock %+.1f ± %.1f ppm against the host "
                "over %.1f s (limit %d)%s; round trip min %.0f us" % (
                    i, f["drift_ppm"], f["drift_se_ppm"] or 0.0, f["span_s"], DRIFT_LIMIT_PPM,
                    "" if ok else ": the timer's rate is not what the firmware assumes", f["rtt_us"]["min"]), fit=f)


# --- the run -------------------------------------------------------------------------------------

def run(a):
    dictionary = S.load_dictionary(a.dict, a.binary)
    funcs = S.elf_functions(a.binary) if a.binary else []
    resets = [] if a.no_reset else [m for m in a.resets.split(",") if m]
    if a.watchdog and not a.no_reset and "watchdog" not in resets:
        resets.append("watchdog")
    for m in resets:
        if m not in S.RESET_HOW:
            raise S.SmclogError("--resets: %s is not one of %s" % (m, ", ".join(S.RESET_HOW)))
    session = Session(a.session, dictionary)
    meta = {"format": S.FORMAT_VERSION, "tool": "firstrun", "created": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "host": platform.platform(), "port": a.port, "build_id": dictionary.get("build_id"),
            "dictionary": a.dict or os.path.basename(a.binary), "resets": resets,
            "t_host_start_ns": time.monotonic_ns()}
    with open(os.path.join(a.session, "meta.json"), "w") as f:
        json.dump(meta, f, indent=1, sort_keys=True)
    rep = Report(a.quiet)
    holder = {}
    try:
        drive(a, rep, session, dictionary, funcs, resets, holder)
    finally:
        step_integrity(rep, session.records)
        step_clock(rep, session.records)
        session.close()
    return finish(a, rep, meta, holder.get("info"))


def drive(a, rep, session, dictionary, funcs, resets, holder):
    dev, info = connect(a.port, session, a.wait)
    if dev is None:
        rep.add("link", "fail", "no answer to GET_INFO on %s within %g s" % (a.port, a.wait))
        return
    holder["info"] = info
    if not step_link(rep, info, dictionary):
        dev.close()
        return
    caps = info["caps"]
    journal = None
    try:
        step_ping(rep, dev)
        if step_stream(rep, dev):
            if "boot_log" in caps:
                journal = step_boot(rep, dev, funcs)
            else:
                rep.add("boot", "skip", "no BOOT_LOG in this firmware")
            step_levels(rep, dev, dictionary)
            if "raw_input" in caps:
                step_raw(rep, dev, info)
            else:
                rep.add("raw", "skip", "no raw input in this firmware")
            if "inject" in caps:
                step_inject(rep, dev)
            else:
                rep.add("inject", "skip", "no injection in this firmware")
            if "metrics" in caps:
                step_metrics(rep, dev)
            else:
                rep.add("metrics", "skip", "no METRICS_NOW in this firmware")
            if a.listen > 0:
                mark = len(dev.records())
                dev.listen(a.listen)
                n = sum(1 for x in dev.records()[mark:] if x["kind"] == "log")
                rep.add("listen", "info", "%g s at rest: %d log records" % (a.listen, n))
            for how in resets:
                if "reset" not in caps:
                    rep.add("reset", "skip", "no RESET in this firmware")
                    break
                dev, journal = step_reset(rep, a, session, dev, info, how, journal, funcs)
                if dev is None:
                    break
        if dev is not None:
            dev.listen(0.2)
            dev.call("unsubscribe")
            dev.close()
    except (OSError, S.SmclogError) as e:
        rep.add("link", "fail", "the link broke off: %s" % e)


def finish(a, rep, meta, info):
    head = ["firstrun %s on %s" % (meta["created"], a.port),
            "build %s, variant %s" % (info["build_id"], info["variant"]) if info else "no device",
            "session %s" % a.session]
    with open(os.path.join(a.session, "report.json"), "w") as f:
        json.dump({"meta": meta, "info": info, "items": rep.items, "failed": len(rep.failed())}, f, indent=1,
                  sort_keys=True)
    text = rep.text(head)
    with open(os.path.join(a.session, "report.txt"), "w") as f:
        f.write(text)
    if not a.quiet:
        print("\n%d passed, %d failed; the report: %s" % (
            sum(1 for i in rep.items if i["status"] == "pass"), len(rep.failed()),
            os.path.join(a.session, "report.txt")))
    return 1 if rep.failed() else 0


def main(argv=None):
    p = argparse.ArgumentParser(prog="firstrun", description="The first run on the device (docs/hwtest.md).")
    p.add_argument("--port", required=True, help="serial device (USB CDC): /dev/cu.usbmodem*, /dev/ttyACM0, COM3, or auto (found by its USB IDs)")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--binary", help="the firmware's ELF: its dictionary, and names for the crash's addresses")
    g.add_argument("--dict", help="dictionary JSON (the crash's addresses stay unnamed)")
    p.add_argument("--session", required=True, help="session directory to write")
    p.add_argument("--resets", default="soft,fault", help="RESET modes to test, in order (default soft,fault)")
    p.add_argument("--watchdog", action="store_true", help="also the watchdog test: the device hangs up to 8 s")
    p.add_argument("--no-reset", action="store_true", help="no RESET at all")
    p.add_argument("--wait", type=float, default=10.0, help="seconds to wait for the device (default 10)")
    p.add_argument("--listen", type=float, default=10.0,
                   help="seconds to listen at rest before the resets, for the clock's drift (default 10)")
    p.add_argument("--quiet", action="store_true", help="print nothing; the report is in the session")
    a = p.parse_args(argv)
    try:
        return run(a)
    except S.SmclogError as e:
        sys.exit("firstrun: %s" % e)


if __name__ == "__main__":
    sys.exit(main())

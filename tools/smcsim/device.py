"""smcsim device: scenarios on the device over the command link (SPEC LOG-35; docs/core.md,
"Scenarios on the device").

A run, for each scenario:

1. The core is restarted on the scenario's configuration and settings (CORE_SETUP, with injection
   on; in RAM only).
2. The scenario's input steps are injected (INPUT_INJECT) at their times, in real time, while the
   device's log streams in (the core's input records, its MIDI out, LED states, page and Shift
   changes).
3. Its expectations are checked against the device's records.
4. The simulator gets the same input at the times the device stamped it, and its MIDI and final
   LED states must equal the device's: the core on the computer and on the device must agree.
5. With --midi, the MIDI the host received on the performance port in the meantime (midiin.py)
   must equal what the device logged it sent: nothing lost, changed or added on the way.
6. The device goes back to the factory configuration and injection off.

Expectation failures with agreement mean the scenario depends on timing the device did not meet
(the report gives how far the inputs were off); disagreement is a bug in one of the two.

Standard library only; called from smcsim.py.
"""
import json
import os
import struct
import tempfile
import time

import corelog
import midiin
import smcsim
import smclog

SETTLE_MS = 300          # after the last step: let the device finish (ticks, LEDs) before comparing
LEAD_MS = 100            # from the setup to the scenario's time 0
SETUP_TIMEOUT_S = 2.0
LEVELS = {"all": 3, "input": 4, "engine": 5}  # INFO; input records DEBUG; MIDI out DEBUG, LEDs TRACE
TICK_US = 10000          # the device's control task runs at least this often

TRANSPORT = {"usb": 0, "ble": 1}


class Skip(Exception):
    """The device cannot run this scenario (the reason is the message)."""


def input_key(e):
    """A comparable form of an input event of Translator.events(): what the core logs."""
    k = e[0]
    if k == "m":
        return ("m", TRANSPORT[e[2]], " ".join(x.lower() for x in e[3]))
    return (k, e[2], e[3])


def record_input(r):
    """(key, injected) for a core input record (module input), or None."""
    if r.get("kind") != "log" or r.get("module") != "input":
        return None
    ev = corelog.parse_input(r.get("msg", ""))
    return (ev[:3], ev[3]) if ev else None


def inject_bytes(e):
    """INPUT_INJECT event bytes (docs/hwtest.md) for an input event."""
    k = e[0]
    if k == "b":
        return bytes([1, e[2], e[3], 0, 0])
    if k == "m":
        return bytes([4, TRANSPORT[e[2]]] + [int(x, 16) for x in e[3]])
    return bytes([{"e": 2, "f": 3}[k], e[2]]) + struct.pack("<h", e[3]) + b"\0"


def plan_scenario(path, names):
    """The scenario's steps, its settings and its events per step; Skip if the device cannot run it."""
    steps = smcsim.load_scenario(path)
    tr = smcsim.Translator(names)
    settings, per_step, ends = {}, [], []
    for i, step in enumerate(steps):
        before = tr.t_us
        events = tr.events(step)
        for e in events:
            if e[0] == "set":
                settings[e[1]] = e[2]
        per_step.append([e for e in events if e[0] in smcsim.INPUTS or e[0] in ("t", "edit", "select")])
        ends.append(max(tr.t_us, before))  # a wait's end; an input step's last input
    # a scenario without "config" means the simulator's default, the test configuration; the
    # device's default is the factory one, so it is always named
    settings = dict({"config": "test"}, **settings)
    return steps, tr, settings, per_step, ends


# --- the device side -----------------------------------------------------------------------------

class TeeLink(smclog.Link):
    """A Link that keeps the raw bytes (the session's device.bin) and every record."""

    def __init__(self, fd, decoder):
        super().__init__(fd, decoder, on_record=self._keep)
        self.raw = bytearray()
        self.records = []

    def _keep(self, r):
        self.records.append(r)

    def read(self, timeout):
        recs, data = super().read(timeout)
        self.raw += data
        return recs, data

    def pump(self, seconds):
        """Reads records for this long (at least one read attempt)."""
        end = time.monotonic() + seconds
        while True:
            recs, _ = self.read(max(0.0, min(0.005, end - time.monotonic())))
            self.records += recs
            if time.monotonic() >= end:
                return

    def wait_until(self, t_mono):
        while time.monotonic() < t_mono:
            recs, _ = self.read(max(0.0, min(0.002, t_mono - time.monotonic())))
            self.records += recs

    def ask(self, name, payload=b""):
        r = self.call(smclog.CMDS.get(name) or smclog.CFG_CMDS[name], payload)
        if r["status"] != "ok":
            raise smcsim.ScenarioError("the device refused %s: %s" % (name, r["status"]))
        return r


class Device:
    """The device behind a serial port: checked, subscribed, its log levels set."""

    def __init__(self, port, dictionary, midi=None):
        self.dictionary = dictionary
        self.midi = midi  # a midiin.Source, or None
        self.fd = smclog.open_serial(port)
        self.link = TeeLink(self.fd, smclog.Decoder(dictionary))
        info = self.link.ask("info")
        if dictionary.get("build_id") and info["build_id"] != dictionary["build_id"]:
            raise smclog.BuildIdMismatch("device build %s, dictionary build %s" %
                                         (info["build_id"], dictionary["build_id"]))
        if info["variant"] != "app":
            raise smcsim.ScenarioError("the device runs the %s variant; scenarios need the controller (app)"
                                       % info["variant"])
        if "inject" not in info["caps"]:
            raise smcsim.ScenarioError("the device does not offer injection")
        self.info = info
        self.levels = self.link.ask("levels").get("levels", [])  # put back by close()
        self.link.ask("subscribe")
        for module, level in LEVELS.items():
            self.link.ask("level", bytes([smclog.module_index(module, dictionary), level]))

    def close(self):
        try:
            if "setup" in self.info["caps"]:
                self.link.ask("setup")  # the factory configuration again (needs injection on)
            self.link.ask("inject", b"\x00")
            for i, level in enumerate(self.levels):
                self.link.ask("level", bytes([i, level]))
            self.link.send(smclog.CMDS["unsubscribe"])
        except (smclog.SmclogError, smcsim.ScenarioError, OSError):
            pass
        os.close(self.fd)

    def edit(self, preset, records):
        """A live edit (CFG_SET) applied; returns the span of link.records it logged: from the
        command to the core's "configuration edited", which ends what the control task does for it."""
        link = self.link
        lo = len(link.records)  # the device may apply it before its reply is read
        r = link.ask("cfg-set", struct.pack("<IB", 0, preset) + records)
        p = bytes.fromhex(r.get("payload", ""))
        if not p or p[0] != 0:
            raise smcsim.ScenarioError("the device refused the edit (preset %d, %s): status %s" % (
                preset + 1, records.hex(), p[0] if p else "none"))
        end = time.monotonic() + 1.0
        while True:
            hi = next((i for i in range(lo, len(link.records)) if link.records[i].get("kind") == "log"
                       and link.records[i].get("module") == "engine"
                       and link.records[i].get("msg", "").startswith("configuration edited")), None)
            if hi is not None:
                return lo, hi + 1
            if time.monotonic() > end:
                raise smcsim.ScenarioError("the device took the edit but logged no \"configuration edited\"")
            link.pump(0.005)

    def select(self, preset, page):
        """A selection (CFG_SELECT, P-3; -1 keeps) applied; returns the span of link.records it
        logged: from the command to the core's "preset P page G" after the control task's "select
        asked"."""
        link = self.link
        lo = len(link.records)
        r = link.ask("cfg-select", bytes([preset & 0xFF, page & 0xFF]))
        p = bytes.fromhex(r.get("payload", ""))
        if not p or p[0] != 0:
            raise smcsim.ScenarioError("the device refused the selection (preset %d, page %d): status %s" % (
                preset + 1, page + 1, p[0] if p else "none"))
        end = time.monotonic() + 1.0
        while True:
            asked = next((i for i in range(lo, len(link.records)) if is_engine(link.records[i], "select asked")), None)
            hi = None if asked is None else next(
                (i for i in range(asked, len(link.records)) if corelog.parse_bank(link.records[i].get("msg", ""))
                 and link.records[i].get("module") == "engine"), None)
            if hi is not None:
                return lo, hi + 1
            if time.monotonic() > end:
                raise smcsim.ScenarioError("the device took the selection but logged no \"preset P page G\"")
            link.pump(0.005)

    def run(self, settings, per_step, settle_ms=SETTLE_MS):
        """Plays the events; returns (records from the core's start, injections, host times of the
        steps in ns, the MIDI the host received from the core's start: [(t_ns, bytes)], or None
        without a MIDI source, the records of each live edit: [(step, lo, hi)] in the records
        returned)."""
        link = self.link
        link.ask("inject", b"\x01")
        if settings != {"config": "factory"} and "setup" not in self.info["caps"]:
            raise Skip("needs settings %s; the device has no CORE_SETUP" % settings)
        text = "\n".join("%s %s" % kv for kv in smcsim.set_pairs(settings)).encode()
        first = len(link.records)
        if "setup" in self.info["caps"]:
            r = link.ask("setup", text)
            if r.get("setup") != "ok":
                raise smcsim.ScenarioError("the device refused the settings %s: %s (pair %d)" % (
                    settings, r.get("setup"), r.get("bad_pair", 0)))
            end = time.monotonic() + SETUP_TIMEOUT_S
            while not any(is_core_start(x) for x in link.records[first:]):
                if time.monotonic() > end:
                    raise smcsim.ScenarioError("no \"core start\" from the device after the setup")
                link.pump(0.01)
        start = next((i for i in range(first, len(link.records)) if is_core_start(link.records[i])), first)
        if self.midi:
            self.midi.mark()
        t0 = time.monotonic() + LEAD_MS / 1000
        injections, step_ns, edits = [], [], []
        for i, events in enumerate(per_step):
            step_ns.append(None)
            inputs = [e for e in events if e[0] in smcsim.INPUTS]
            k = 0
            while k < len(inputs):  # events at the same time go in one command
                n = 1
                while k + n < len(inputs) and inputs[k + n][1] == inputs[k][1] and n < 32:
                    n += 1
                group = inputs[k:k + n]
                link.wait_until(t0 + group[0][1] / 1e6)
                if step_ns[-1] is None:
                    step_ns[-1] = time.monotonic_ns()
                r = link.ask("inject", b"\x02" + b"".join(inject_bytes(e) for e in group))
                injections.append((group, r))
                k += n
            for e in events:
                if e[0] in ("edit", "select"):
                    link.wait_until(t0 + e[1] / 1e6)
                    if step_ns[-1] is None:
                        step_ns[-1] = time.monotonic_ns()
                    lo, hi = self.edit(e[2], bytes.fromhex(e[3])) if e[0] == "edit" else self.select(e[2], e[3])
                    edits.append((i, lo - start, hi - start))
            last = events[-1][1] if events else None
            if last is not None and not inputs:  # a wait: let its time pass
                link.wait_until(t0 + last / 1e6)
        link.pump(settle_ms / 1000)
        host_midi = self.midi.take() if self.midi else None
        return link.records[start:], injections, step_ns, host_midi, edits


def is_engine(r, prefix):
    return r.get("kind") == "log" and r.get("module") == "engine" and r.get("msg", "").startswith(prefix)


def is_core_start(r):
    return r.get("kind") == "log" and r.get("module") == "engine" and r.get("msg", "").startswith("core start:")


# --- comparing -------------------------------------------------------------------------------------

def compare_host_midi(name, host_midi, midi_dev):
    """The MIDI the host received against what the device logged it sent; failures."""
    host = [midiin.hex_of(m) for _, m in host_midi]
    if host == midi_dev:
        return []
    k = next((i for i, (a, b) in enumerate(zip(host, midi_dev)) if a != b), min(len(host), len(midi_dev)))
    return ["%s: the host received other MIDI than the device logged, at message %d: host %s, device %s "
            "(%d received, %d logged)" % (name, k, host[k] if k < len(host) else "nothing",
                                          midi_dev[k] if k < len(midi_dev) else "nothing", len(host), len(midi_dev))]


def analyse(path, steps, tr, settings, per_step, ends, records, injections, sim, host_midi=None, edits=()):
    """Returns (failures, notes, details). host_midi: what the host received, or None. edits: each
    live edit's step and the span of records it logged ([(step, lo, hi)], Device.run())."""
    failures, notes = [], []
    name = os.path.basename(path)
    for group, r in injections:
        if r.get("inject") != "ok" or r.get("accepted") != len(group):
            failures.append("%s: the device refused injected input: %s, %d of %d accepted" % (
                name, r.get("inject"), r.get("accepted", 0), len(group)))
    broken = [r for r in records if r.get("kind") in ("lost", "gap", "bad_frame")]
    if broken:
        return failures + ["%s: the device log is incomplete (%s); nothing compared" % (
            name, ", ".join(sorted({r["kind"] for r in broken})))], notes, {}
    edit_step = {id(records[j]): i for i, lo, hi in edits for j in range(lo, hi)}
    logs = [r for r in records if r.get("kind") == "log"]

    # the device's input records against the scenario's, one for one
    plan = [(i, e) for i, events in enumerate(per_step) for e in events if e[0] in smcsim.INPUTS]
    seen = []
    for r in logs:
        x = record_input(r)
        if x is None:
            continue
        if not x[1]:
            return failures + ["%s: real input during the run (%s); nothing compared" % (name, r["msg"])], notes, {}
        seen.append((x[0], r))
    want = [input_key(e) for _, e in plan]
    got = [k for k, _ in seen]
    if got != want:
        k = next((j for j, (a, b) in enumerate(zip(got, want)) if a != b), min(len(got), len(want)))
        return failures + ["%s: the device's input differs from the scenario's at input %d: device %s, scenario %s" % (
            name, k, got[k] if k < len(got) else "nothing", want[k] if k < len(want) else "nothing")], notes, {}

    # Pair every timed action with its device record before choosing one common
    # origin. An edit or selection can precede the first injected input.
    actions = [(i, e, r) for (i, e), (_, r) in zip(plan, seen)]
    for kind, prefix, label in (("edit", "edit: preset", "live edits"),
                                ("select", "select asked", "selections")):
        planned = [(i, e) for i, events in enumerate(per_step) for e in events if e[0] == kind]
        applied = [r for r in logs if is_engine(r, prefix)]
        if len(applied) != len(planned):
            return failures + ["%s: %d %s sent, the device logged %d" % (
                name, len(planned), label, len(applied))], notes, {}
        actions += [(i, e, r) for (i, e), r in zip(planned, applied)]
    actions.sort(key=lambda x: (x[0], x[1][1]))
    # Align the earliest recorded action to its planned time; translate all
    # actions, observations and step boundaries by exactly the same amount.
    # This preserves actual intervals (including host jitter), without clamping.
    if actions:
        _, first, record = min(actions, key=lambda x: x[2]["t_us"])
        base = record["t_us"] - first[1]
    else:
        base = (logs[0]["t_us"] if logs else 0) - LEAD_MS * 1000
    dev = [r["t_us"] - base for _, r in seen]
    late = [d - e[1] for d, (_, e) in zip(dev, plan)]
    # Step boundaries follow the latest applied action, including edits/selects;
    # waits retain their planned duration and expectations take no time.
    dev_end, j, anchor = [], 0, (0, 0)
    for i in range(len(steps)):
        while j < len(actions) and actions[j][0] == i:
            _, e, r = actions[j]
            anchor = (e[1], r["t_us"] - base)
            j += 1
        dev_end.append(anchor[1] + ends[i] - anchor[0])

    # the device's records by step: an input record and what its event caused (the same cause ID,
    # LOG-04) at the input's step; anything else (ticks: blinking, timeouts) at the first step that
    # had not ended by then, never before the latest input's step
    obs = [{"midi": [], "leds": [], "state": None, "logs": []} for _ in steps]
    state = {"preset": 1, "page": 1, "shift": False}
    input_steps = [i for i, _ in plan]
    cur, n_in, midi_dev, leds_dev, caused = 0, 0, [], {}, {}
    for r in logs:
        if record_input(r) is not None:
            cur = input_steps[n_in]
            n_in += 1
            if r.get("cause"):
                caused[r["cause"]] = cur
            where = cur
        elif r.get("cause") in caused:
            where = caused[r["cause"]]  # buffered input output can arrive during an edit
        elif id(r) in edit_step:  # what a live edit logged belongs to its step
            cur = where = edit_step[id(r)]
        else:
            limit = input_steps[n_in] if n_in < len(input_steps) else len(steps)
            t = r["t_us"] - base
            where = cur
            while where + 1 < limit and dev_end[where] < t:
                where += 1
        o = obs[where]
        o["logs"].append(r)
        if r.get("module") != "engine":
            continue
        msg = r.get("msg", "")
        out = corelog.parse_out(msg)
        if out:
            o["midi"].append(out)
            midi_dev.append(out)
        led = corelog.parse_led(msg)
        if led:
            o["leds"].append(led)
            leds_dev[led[0]] = led[1]
        bank = corelog.parse_bank(msg)
        if bank:
            state = dict(state, preset=bank[0], page=bank[1])
        shift = corelog.parse_shift(msg)
        if shift is not None:
            state = dict(state, shift=shift)
        o["state"] = dict(state)
    st = {"preset": 1, "page": 1, "shift": False}
    for o in obs:  # steps without records keep the state before them
        st = o["state"] = o["state"] or st
    expect = smcsim.check_steps(path, steps, obs, tr)
    failures += ["device: " + f for f in expect]

    # the simulator on the device's timing
    cmds = smcsim.set_commands(settings) + ["level all 2"]
    timed = []
    for _, e, r in actions:
        t = r["t_us"] - base
        # Every input is injected on the device: mark it here too.
        if e[0] in smcsim.INPUTS:
            e = e[:-1] + (True,)
        timed.append((t, 0, smcsim.sim_command(e, t)))
    end = max(dev + dev_end[-1:] + [(logs[-1]["t_us"] - base) if logs else 0])
    t = TICK_US
    while t <= end + TICK_US:
        timed.append((t, 1, "t %d" % t))
        t += TICK_US
    if min([x[0] for x in timed] + [0]) < 0:
        raise smcsim.ScenarioError("device times before the scenario's start")
    cmds += [c for _, _, c in sorted(timed, key=lambda x: (x[0], x[1]))]
    with tempfile.TemporaryDirectory() as tmp:
        lines = smcsim.run_sim(sim, cmds, os.path.join(tmp, "sim.bin"))
    midi_sim = [" ".join(line.split()[3:]) for line in lines if line.startswith("M ")]
    leds_sim = {}
    for line in lines:
        if line.startswith("L "):
            p = line.split()
            leds_sim[int(p[2])] = int(p[3])
    agree = True
    if midi_sim != midi_dev:
        agree = False
        k = next((i for i, (a, b) in enumerate(zip(midi_sim, midi_dev)) if a != b), min(len(midi_sim), len(midi_dev)))
        failures.append("%s: the device and the simulator disagree at MIDI message %d: device %s, simulator %s" % (
            name, k, midi_dev[k] if k < len(midi_dev) else "nothing", midi_sim[k] if k < len(midi_sim) else "nothing"))
    if leds_dev:
        diff = sorted(i for i in set(leds_sim) | set(leds_dev) if leds_sim.get(i) != leds_dev.get(i))
        if diff:
            agree = False
            led_names = {v: k for k, v in tr.names["l"].items()}
            failures.append("%s: the device and the simulator disagree on LEDs at the end: %s" % (name, ", ".join(
                "%s device %s simulator %s" % (led_names.get(i, i), smcsim.LED_NAMES.get(leds_dev.get(i), "?"),
                                               smcsim.LED_NAMES.get(leds_sim.get(i), "?")) for i in diff)))
    else:
        notes.append("no LED records from the device (engine log level below TRACE): LEDs not compared")
    if host_midi is not None:
        failures += compare_host_midi(name, host_midi, midi_dev)
    worst = max((abs(x) for x in late), default=0) // 1000
    if expect and agree:
        notes.append("the device and the simulator agree; the expectations failed on the device's timing "
                     "(inputs up to %d ms off the scenario's times)" % worst)
    details = {"inputs": len(plan), "messages": len(midi_dev), "late_ms": worst, "agree": agree}
    if host_midi is not None:
        details["received"] = len(host_midi)
    return failures, notes, details


def run_on_device(dev, path, sim, names, session=None, settle_ms=SETTLE_MS):
    """One scenario on an open Device; returns (ok, failures, notes, details). Raises Skip."""
    steps, tr, settings, per_step, ends = plan_scenario(path, names)
    raw_from = len(dev.link.raw)
    records, injections, step_ns, host_midi, edits = dev.run(settings, per_step, settle_ms)
    failures, notes, details = analyse(path, steps, tr, settings, per_step, ends, records, injections, sim,
                                       host_midi, edits)
    ok = not failures
    if session:
        write_session(session, dev, path, settings, bytes(dev.link.raw[raw_from:]), records, steps, step_ns,
                      ok, failures, notes, host_midi)
    details["steps"] = len(steps)
    return ok, failures, notes, details


def write_session(d, dev, scenario, settings, raw, records, steps, step_ns, ok, failures, notes, host_midi=None):
    os.makedirs(d, exist_ok=True)
    meta = {"format": 1, "tool": "smcsim device", "source": "device", "scenario": os.path.relpath(scenario, smcsim.ROOT),
            "build_id": dev.info["build_id"], "variant": dev.info["variant"], "settings": settings,
            "configuration": "core_config_" + settings.get("config", "factory"),
            "created": time.strftime("%Y-%m-%dT%H:%M:%S%z")}
    with open(os.path.join(d, "meta.json"), "w") as f:
        json.dump(meta, f, indent=2, sort_keys=True)
        f.write("\n")
    with open(os.path.join(d, "device.bin"), "wb") as f:
        f.write(raw)
    with open(os.path.join(d, "device.jsonl"), "w") as f:
        for r in records:
            f.write(json.dumps(r, sort_keys=True) + "\n")
    if host_midi is not None:  # LOG-21's midi.jsonl: what the host received, on its monotonic clock
        with open(os.path.join(d, "midi.jsonl"), "w") as f:
            for t_ns, m in host_midi:
                b = list(m)
                f.write(json.dumps({"t_ns": t_ns, "dir": "rx", "src": "host", "hex": midiin.hex_of(m),
                                    "msg": smcsim.describe(b + [0] * (3 - len(b)))}, sort_keys=True) + "\n")
    with open(os.path.join(d, "markers.jsonl"), "w") as f:
        for i, s in enumerate(steps):
            text = {k: v for k, v in s.items() if k != "_line"}
            f.write(json.dumps({"step": i, "line": s["_line"], "t_ns": step_ns[i], "text": json.dumps(text, sort_keys=True)},
                               sort_keys=True) + "\n")
    with open(os.path.join(d, "result.json"), "w") as f:
        json.dump({"ok": ok, "failures": failures, "notes": notes}, f, indent=2)
        f.write("\n")


def scenario_files(paths):
    out = []
    for p in paths:
        if os.path.isdir(p):
            out += sorted(os.path.join(p, f) for f in os.listdir(p) if f.endswith(".jsonl"))
        else:
            out.append(p)
    if not out:
        raise smcsim.ScenarioError("no scenarios in %s" % " ".join(paths))
    return out


def main(args):
    """smcsim device: returns the exit status."""
    dictionary = smclog.load_dictionary(args.dict, args.binary)
    names = smcsim.sim_names(args.sim)
    files = scenario_files(args.scenarios)
    try:
        midi = midiin.open_source(args.midi) if args.midi else None
    except midiin.MidiError as e:
        raise smcsim.ScenarioError(str(e))
    try:
        dev = Device(args.port, dictionary, midi)
    except BaseException:
        if midi:
            midi.close()
        raise
    bad = skipped = 0
    try:
        for path in files:
            name = os.path.basename(path)
            session = os.path.join(args.session, os.path.splitext(name)[0]) if args.session else None
            try:
                ok, failures, notes, d = run_on_device(dev, path, args.sim, names, session, args.settle_ms)
            except Skip as e:
                skipped += 1
                print("skip %s: %s" % (name, e))
                continue
            print("%s %s (%d steps, %d inputs, %d MIDI messages%s; inputs up to %d ms off)" % (
                "ok  " if ok else "FAIL", name, d.get("steps", 0), d.get("inputs", 0), d.get("messages", 0),
                ", %d received by the host" % d["received"] if "received" in d else "", d.get("late_ms", 0)))
            for f in failures:
                print("     " + f)
            for n in notes:
                print("     note: " + n)
            bad += not ok
    finally:
        dev.close()
        if midi:
            midi.close()
    print("smcsim device: %d scenarios, %d failed, %d skipped" % (len(files), bad, skipped))
    return 1 if bad else 0

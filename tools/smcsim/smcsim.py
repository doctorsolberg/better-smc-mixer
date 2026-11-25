"""smcsim: runs scenarios through the core simulator (docs/core.md; SPEC LOG-35, LOG-40, LOG-42).

Usage: python3 -I tools/smcsim/smcsim.py COMMAND ...

  run SCENARIO [--session DIR] [--sim BINARY] [--level N] [-v]
        Run one scenario file (JSON Lines) through the simulator, check its expectations and,
        with --session, write a session directory in the LOG-21 format (meta.json, device.bin,
        device.jsonl, midi.jsonl, markers.jsonl) plus result.json.
  run-all DIR [--sim BINARY] [--level N]
        Run every *.jsonl in DIR (the regression corpus, LOG-42); exit 1 if any fails.
  device SCENARIO|DIR... --port DEV (--dict DICT.json | --binary BINARY) [--session DIR]
         [--sim BINARY] [--settle-ms N] [--midi raw:PATH|coremidi:NAME]
        Run scenarios on the device (the controller, app variant) over the command link (LOG-35):
        the core restarted on each scenario's settings (CORE_SETUP), its inputs injected in real
        time, its expectations checked on the device's log, and the simulator, fed the device's own
        input times, must agree with the device's MIDI and LEDs (device.py). --midi: the MIDI the
        host receives on the performance port must equal what the device logged (midiin.py: raw:PATH
        for a rawmidi device or a stream, coremidi:NAME on macOS, "SMC-Mixer"). --session DIR writes
        a session per scenario (with --midi, midi.jsonl: what the host received). Without hardware:
        --port on build/host/devhost behind a pseudo-terminal, --midi raw: on a FIFO it writes to
        (test_smcsim.py).
  replay SESSION [--sim BINARY]
        Feed the input events recorded in a session's device log (module input, LOG-41 at the
        core's level) to the simulator and compare its MIDI with the MIDI the session's core sent
        (module engine, "out ..."): the core on the device and on the computer must agree (LOG-35).

The simulator binary is build/host/smcsim (make -C firmware sim). Its log is decoded with its own
dictionary (tools/smclog). Standard library only.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "smclog"))
sys.path.insert(0, os.path.join(ROOT, "tools", "smcconf"))
sys.path.insert(0, HERE)  # python3 -I leaves the script's directory out
import codec  # noqa: E402
import corelog  # noqa: E402
import smclog  # noqa: E402

DEFAULT_SIM = os.path.join(ROOT, "build", "host", "smcsim")
FADER_MAX = 16383
STEP_MS = 10          # an input step without "after_ms" comes this long after the previous one
TICK_MS = 10          # wait_ms sends a tick this often
LED_STATES = {"off": 0, "on": 1, "slow": 2, "fast": 3}
LED_NAMES = {v: k for k, v in LED_STATES.items()}
PRESET_SETTINGS = ("shift_mode", "note_off", "channel", "param_null")  # each also as NAME_p1 ... NAME_p4 (one preset)
SETTINGS = ({"config", "layer_mode", "pickup", "gesture_ms", "hybrid_ms", "enc_accel", "fine14"}
            | set(PRESET_SETTINGS)
            | {"%s_p%d" % (name, k) for name in PRESET_SETTINGS for k in range(1, 5)})


# The panel's names (core_names.c) of the strip controls, by group of the binary form (codec.STRIP_KEYS)
EDIT_GROUPS = (("fader", "fader"), ("enc", "encoder"), ("m", "m"), ("s", "s"), ("r", "r"), ("sel", "select"))
EDIT_ROW = ("play", "stop", "rec", "rew", "ff", "bank_left", "bank_right", "up", "down", "left", "right")


def edit_records(edit):
    """An edit step's records of the binary form (docs/config.md, "Edits"; CFG_SET) and its preset
    (0-based): {"preset": P, "page": G, "control": NAME, "to": {a control as in a file}}, the
    control replaced whole ("to": {} sends nothing); {"preset": P, "settings": {...}}, the preset's
    settings replaced whole; {"global": {...}}, the global settings replaced whole."""
    try:
        if "global" in edit:
            m = codec.from_json({"schemaVersion": codec.S.version, "global": edit["global"]})
            return 0, b"".join(r for r, _ in codec.global_records(m))  # every global record, whole
        p = int(edit.get("preset", 1)) - 1
        if not 0 <= p < 4:
            raise ScenarioError("edit: preset is 1-4")
        if "settings" in edit:
            presets = [{} for _ in range(p)] + [edit["settings"]]
            m = codec.from_json({"schemaVersion": codec.S.version, "presets": presets})
            return p, codec.preset_record(p, m["presets"][p])[0]
        name = edit.get("control")
        if name in EDIT_ROW:
            page, cid, key, path = 255, EDIT_ROW.index(name), None, {"row": {codec.ROW_KEYS[EDIT_ROW.index(name)]: edit.get("to", {})}}
        else:
            hit = [(g, key) for g, (pre, key) in enumerate(EDIT_GROUPS)
                   if name and name.startswith(pre) and name[len(pre):].isdigit()]
            if not hit or not 1 <= int(name[len(EDIT_GROUPS[hit[0][0]][0]):]) <= 8:
                raise ScenarioError("edit: no control named %r" % name)
            g, key = hit[0]
            n = int(name[len(EDIT_GROUPS[g][0]):]) - 1
            page = int(edit.get("page", 1)) - 1
            if not 0 <= page < 4:
                raise ScenarioError("edit: page is 1-4")
            cid = g * 8 + n
            strips = [{} for _ in range(n)] + [{key: edit.get("to", {})}]
            path = {"pages": [{} for _ in range(page)] + [{"strips": strips}]}
        m = codec.from_json({"schemaVersion": codec.S.version, "presets": [{} for _ in range(p)] + [path]})
        ctl = None
        for g2, c2, kind, nav, c in codec.controls(m["presets"][p]):
            if (g2, c2) == (page, cid):
                ctl = codec.control_records(g2, c2, kind, c) or bytes([3, 4, g2, c2, 0, 0])
        return p, ctl
    except codec.ConfigError as e:
        raise ScenarioError("edit: %s" % e)


def set_pairs(settings):
    """(NAME, VALUE) in the order they are applied: "config" first, since it replaces the whole
    configuration (firmware/core/core_settings.h)."""
    return [(k, settings[k]) for k in sorted(settings, key=lambda k: (k != "config", k))]


def set_commands(settings):
    """smcsim "set" commands."""
    return ["set %s %s" % (k, v) for k, v in set_pairs(settings)]


class ScenarioError(Exception):
    pass


def describe(m):
    """The same text as smcmidi's describe()."""
    s, ch = m[0], (m[0] & 15) + 1
    kind = s & 0xF0
    if kind == 0x80:
        return "note_off ch%d %d %d" % (ch, m[1], m[2])
    if kind == 0x90:
        return ("note_off ch%d %d 0" % (ch, m[1])) if m[2] == 0 else "note_on ch%d %d %d" % (ch, m[1], m[2])
    if kind == 0xA0:
        return "poly_at ch%d %d %d" % (ch, m[1], m[2])
    if kind == 0xB0:
        return "cc ch%d %d %d" % (ch, m[1], m[2])
    if kind == 0xC0:
        return "pc ch%d %d" % (ch, m[1])
    if kind == 0xD0:
        return "chan_at ch%d %d" % (ch, m[1])
    if kind == 0xE0:
        return "pitch ch%d %d" % (ch, m[1] | m[2] << 7)
    return "sys %02X" % s


def norm_hex(text):
    """'b0 14 64' -> 'B0 14 64'; checks it is hex bytes."""
    parts = text.split()
    if not parts or any(not re.fullmatch(r"[0-9A-Fa-f]{2}", p) for p in parts):
        raise ScenarioError("not a MIDI message in hex: %r" % text)
    return " ".join(p.upper() for p in parts)


def pos_7bit(v):
    """Fader position for a value on a 0-127 range (the rounding the tests use)."""
    if not 0 <= v <= 127:
        raise ScenarioError("fader value %r is not 0-127" % v)
    return (v * FADER_MAX + 63) // 127


# --- the simulator process ---------------------------------------------------------------------

def sim_names(sim):
    out = subprocess.run([sim], input="names\nquit\n", capture_output=True, text=True, check=True).stdout
    names = {"b": {}, "e": {}, "f": {}, "l": {}}
    for line in out.splitlines():
        p = line.split()
        if p and p[0] == "N":
            names[p[1]][p[3]] = int(p[2])
    return names


def run_sim(sim, commands, log_path):
    r = subprocess.run([sim, "--log", log_path], input="".join(c + "\n" for c in commands),
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise ScenarioError("simulator failed (%d): %s" % (r.returncode, r.stderr.strip()))
    return r.stdout.splitlines()


def decode_log(sim, log_path):
    dictionary = smclog.make_dictionary(sim)
    dec = smclog.Decoder(dictionary)
    with open(log_path, "rb") as f:
        return [r for r in dec.feed(f.read())]


# --- scenarios -----------------------------------------------------------------------------------

def load_scenario(path):
    steps = []
    with open(path) as f:
        for n, line in enumerate(f, 1):
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            try:
                step = json.loads(s)
            except json.JSONDecodeError as e:
                raise ScenarioError("%s:%d: %s" % (path, n, e))
            if not isinstance(step, dict):
                raise ScenarioError("%s:%d: a step is a JSON object" % (path, n))
            step["_line"] = n
            steps.append(step)
    return steps


class Translator:
    """Turns scenario steps into simulator commands at virtual times."""

    def __init__(self, names):
        self.names = names
        self.t_us = 0
        self.started = False

    def index(self, kind, name):
        if isinstance(name, int):
            return name
        if name not in self.names[kind]:
            raise ScenarioError("no %s named %r" % ({"b": "button", "e": "encoder", "f": "fader",
                                                      "l": "LED"}[kind], name))
        return self.names[kind][name]

    def advance(self, step, default_ms=STEP_MS):
        self.t_us += int(step.get("after_ms", default_ms) * 1000)
        self.started = True
        return self.t_us

    def events(self, step):
        """Events for one step at virtual times (expectations add none):
        ("set", NAME, VALUE), ("b", T, BUTTON, 0|1, INJ), ("e", T, ENCODER, CLICKS, INJ),
        ("f", T, FADER, POSITION, INJ), ("m", T, "usb"|"ble", [B0, B1, B2], INJ), ("t", T),
        ("select", T, PRESET, PAGE) (0-based, -1 keeps), ("edit", T, PRESET, HEX) (0-based, the
        records), ("state",)."""
        out = []
        inj = bool(step.get("injected"))
        if "set" in step:
            if self.started:
                raise ScenarioError("'set' must come before any input step")
            for k in step["set"]:
                if k not in SETTINGS:
                    raise ScenarioError("unknown setting %r" % k)
            out.extend(("set", k, v) for k, v in set_pairs(step["set"]))
        elif "press" in step or "release" in step:
            down = "press" in step
            b = self.index("b", step["press" if down else "release"])
            out.append(("b", self.advance(step), b, 1 if down else 0, inj))
        elif "tap" in step:
            b = self.index("b", step["tap"])
            out.append(("b", self.advance(step), b, 1, inj))
            self.t_us += int(step.get("hold_ms", 50) * 1000)
            out.append(("b", self.t_us, b, 0, inj))
        elif "turn" in step:
            e = self.index("e", step["turn"])
            clicks = int(step.get("clicks", 1))
            every = step.get("every_ms", 50)
            for k in range(abs(clicks)):
                t = self.advance(step) if k == 0 else self.advance({}, every)
                out.append(("e", t, e, 1 if clicks > 0 else -1, inj))
        elif "fader" in step:
            f = self.index("f", step["fader"])
            to = step["to_pos"] if "to_pos" in step else pos_7bit(step["to"]) if "to" in step else None
            if to is None:
                raise ScenarioError("a fader step needs 'to' (0-127) or 'to_pos' (0-16383)")
            frm = step["from_pos"] if "from_pos" in step else pos_7bit(step["from"]) if "from" in step else None
            n = int(step.get("steps", 1))
            t = self.advance(step)
            if frm is not None:
                out.append(("f", t, f, frm, inj))
                for k in range(1, n + 1):
                    t = self.advance({}, step.get("every_ms", STEP_MS))
                    out.append(("f", t, f, frm + (to - frm) * k // n, inj))
            else:
                out.append(("f", t, f, to, inj))
        elif "midi_in" in step:
            m = norm_hex(step["midi_in"]).split()
            if len(m) > 3:
                raise ScenarioError("midi_in: at most 3 bytes")
            m += ["00"] * (3 - len(m))
            out.append(("m", self.advance(step), step.get("transport", "usb"), m, inj))
        elif "wait_ms" in step:
            end = self.t_us + int(step["wait_ms"] * 1000)
            while self.t_us < end:
                self.t_us = min(self.t_us + TICK_MS * 1000, end)
                out.append(("t", self.t_us))
            self.started = True
        elif "select" in step:
            sel = step["select"]
            out.append(("select", self.advance(step), sel.get("preset", 0) - 1, sel.get("page", 0) - 1))
        elif "edit" in step:
            p, rec = edit_records(step["edit"])
            out.append(("edit", self.advance(step), p, rec.hex()))
        elif "expect_state" in step:
            out.append(("state",))
        elif not any(k in step for k in ("say", "expect_midi", "expect_led", "expect_log")):
            raise ScenarioError("unknown step %s" % {k: v for k, v in step.items() if k != "_line"})
        return out

    def commands(self, step):
        """Simulator commands for one step."""
        return [sim_command(e) for e in self.events(step)]


INPUTS = ("b", "e", "f", "m")  # the event kinds that are input (injected on the device)


def sim_command(e, t=None):
    """The simulator command for an event of Translator.events(); t replaces its time."""
    k = e[0]
    if k == "set":
        return "set %s %s" % (e[1], e[2])
    if k == "state":
        return "state"
    t = e[1] if t is None else t
    if k == "t":
        return "t %d" % t
    if k == "select":
        return "select %d %d %d" % (t, e[2], e[3])
    if k == "edit":
        return "edit %d %d %s" % (t, e[2], e[3])
    inj = " inj" if e[-1] else ""
    if k == "m":
        return "m %d %s %s%s" % (t, e[2], " ".join(e[3]), inj)
    return "%s %d %d %d%s" % (k, t, e[2], e[3], inj)


def check_steps(path, steps, obs, tr, verbose=False):
    """Checks a scenario's expectations against what was observed at each step; returns failures.
    obs[i] for step i: {"midi": [HEX...], "leds": [(LED, STATE)...], "state": {preset, page, shift}
    or None, "logs": [log records]}. MIDI and log records are checked since the last expectation of
    their kind, LEDs by their state so far."""
    failures = []
    midi_pending, log_pending = [], []
    leds = {}
    for i, step in enumerate(steps):
        where = "%s:%d" % (os.path.basename(path), step["_line"])
        o = obs[i]
        midi_pending += o["midi"]
        for led, state in o["leds"]:
            leds[led] = state
        log_pending += o["logs"]
        state = o["state"]
        if "say" in step and verbose:
            print("  say: %s" % step["say"])
        if "expect_midi" in step:
            want = [norm_hex(m) for m in step["expect_midi"]]
            if midi_pending != want:
                failures.append("%s: MIDI\n      got:  %s\n      want: %s" % (where, midi_pending, want))
            midi_pending = []
        if "expect_led" in step:
            led = tr.index("l", step["expect_led"])
            want = step.get("state", "on")
            if want not in LED_STATES:
                raise ScenarioError("%s: LED state is one of %s" % (where, ", ".join(LED_STATES)))
            got = LED_NAMES.get(leds.get(led), "unknown")
            if got != want:
                failures.append("%s: LED %s is %s, want %s" % (where, step["expect_led"], got, want))
        if "expect_state" in step:
            for k, v in step["expect_state"].items():
                if k not in ("preset", "page", "shift"):
                    raise ScenarioError("%s: expect_state takes preset, page, shift" % where)
                if state is None or state[k] != v:
                    failures.append("%s: %s is %s, want %s" % (where, k, state and state[k], v))
        if "expect_log" in step:
            rx = re.compile(step["expect_log"])
            mod = step.get("module")
            hit = [r for r in log_pending if r.get("kind") == "log" and (mod is None or r.get("module") == mod)
                   and rx.search(r.get("msg", ""))]
            if not hit:
                failures.append("%s: no log record matches %r%s since the last expect_log" % (
                    where, step["expect_log"], " in module %s" % mod if mod else ""))
            log_pending = []
    left = notes_left_on(m for o in obs for m in o["midi"])
    if left:
        failures.append("%s: end: notes left on: %s (on the device the next scenario's set-up "
                        "restarts the core with no Note Off, so they keep sounding in the receiver)" % (
                            os.path.basename(path),
                            ", ".join("channel %d note %d" % (c + 1, n) for c, n in left)))
    return failures


def notes_left_on(messages):
    """The (channel, note) pairs a stream of hex messages leaves on: a Note On with a velocity and no
    Note Off (8n, or 9n with velocity 0) after it."""
    on = set()
    for m in messages:
        b = [int(x, 16) for x in m.split()]
        if len(b) != 3 or b[0] & 0xE0 != 0x80:
            continue
        key = (b[0] & 0x0F, b[1])
        if b[0] & 0xF0 == 0x90 and b[2]:
            on.add(key)
        else:
            on.discard(key)
    return sorted(on)


def run_scenario(path, sim=DEFAULT_SIM, session=None, level=4, verbose=False):
    """Runs a scenario; returns (ok, failures, details)."""
    steps = load_scenario(path)
    names = sim_names(sim)
    tr = Translator(names)
    commands = ["level all %d" % level]
    for i, step in enumerate(steps):
        commands.append("step %d" % i)
        commands += tr.commands(step)
    commands.append("step %d" % len(steps))

    with tempfile.TemporaryDirectory() as tmp:
        log_path = os.path.join(tmp, "device.bin")
        lines = run_sim(sim, commands, log_path)
        records = decode_log(sim, log_path)
        with open(log_path, "rb") as f:
            raw_log = f.read()

    # partition the simulator's output by step markers
    per_step = {i: [] for i in range(len(steps) + 1)}
    cur = None
    warnings = []
    for line in lines:
        p = line.split()
        if not p:
            continue
        if p[0] == "#":
            cur = int(p[1])
        elif p[0] == "W":
            warnings.append(line)
        elif cur is not None:
            per_step[cur].append(p)
    if warnings:
        raise ScenarioError("the simulator did not understand: %s" % "; ".join(warnings))

    # log records by step (the "step N" records of module test)
    log_steps = {i: [] for i in range(len(steps) + 1)}
    cur = None
    for r in records:
        if r.get("kind") == "log" and r.get("module") == "test":
            m = re.fullmatch(r"step (\d+)", r.get("msg", ""))
            if m:
                cur = int(m.group(1))
                continue
        if cur is not None:
            log_steps[cur].append(r)

    obs, midi_all = [], []
    for i in range(len(steps)):
        o = {"midi": [], "leds": [], "state": None, "logs": log_steps[i]}
        for p in per_step[i]:
            if p[0] == "M":
                msg = " ".join(p[3:])
                o["midi"].append(msg)
                midi_all.append((int(p[1]), int(p[2]), msg))
            elif p[0] == "L":
                o["leds"].append((int(p[2]), int(p[3])))
            elif p[0] == "S":
                o["state"] = {"preset": int(p[1]) + 1, "page": int(p[2]) + 1, "shift": p[3] == "1"}
        obs.append(o)
    failures = check_steps(path, steps, obs, tr, verbose)

    ok = not failures
    if session:
        settings = {}
        for step in steps:
            settings.update(step.get("set", {}))
        write_session(session, path, sim, raw_log, records, midi_all, steps, settings, ok, failures)
    return ok, failures, {"messages": len(midi_all), "records": len(records), "steps": len(steps)}


def write_session(d, scenario, sim, raw_log, records, midi, steps, settings, ok, failures):
    os.makedirs(d, exist_ok=True)
    dictionary = smclog.make_dictionary(sim)
    meta = {"format": 1, "tool": "smcsim", "source": "simulator", "scenario": os.path.relpath(scenario, ROOT),
            "binary": os.path.relpath(sim, ROOT), "build_id": dictionary.get("build_id"),
            "configuration": "core_config_test", "settings": settings,
            "clock": "virtual: t_ns = t_us * 1000",
            "t_host_start_ns": 0}
    with open(os.path.join(d, "meta.json"), "w") as f:
        json.dump(meta, f, indent=2, sort_keys=True)
        f.write("\n")
    with open(os.path.join(d, "device.bin"), "wb") as f:
        f.write(raw_log)
    with open(os.path.join(d, "device.jsonl"), "w") as f:
        for r in records:
            r = dict(r)
            if "t_us" in r:
                r["t_host_ns"] = r["t_us"] * 1000
            f.write(json.dumps(r, sort_keys=True) + "\n")
    with open(os.path.join(d, "midi.jsonl"), "w") as f:
        for t_us, cause, hexs in midi:
            b = [int(x, 16) for x in hexs.split()]
            f.write(json.dumps({"t_ns": t_us * 1000, "dir": "rx", "src": "smcsim", "hex": hexs,
                                "msg": describe(b + [0] * (3 - len(b))), "cause": cause}, sort_keys=True) + "\n")
    with open(os.path.join(d, "markers.jsonl"), "w") as f:
        for i, s in enumerate(steps):
            text = {k: v for k, v in s.items() if k != "_line"}
            f.write(json.dumps({"step": i, "line": s["_line"], "text": json.dumps(text, sort_keys=True)},
                               sort_keys=True) + "\n")
    with open(os.path.join(d, "result.json"), "w") as f:
        json.dump({"ok": ok, "failures": failures}, f, indent=2)
        f.write("\n")


# --- replay (LOG-41 at the core's level) -----------------------------------------------------------

def replay(session, sim=DEFAULT_SIM):
    """Returns (ok, details)."""
    with open(os.path.join(session, "device.jsonl")) as f:
        records = [json.loads(line) for line in f if line.strip()]
    settings = {}
    if os.path.exists(os.path.join(session, "meta.json")):
        with open(os.path.join(session, "meta.json")) as f:
            settings = json.load(f).get("settings", {})
    commands, want = set_commands(settings), []
    commands.append("level all 4")
    for r in records:
        if r.get("kind") != "log":
            continue
        msg, t = r.get("msg", ""), r.get("t_us", 0)
        if r.get("module") == "input":
            ev = corelog.parse_input(msg)  # the simulator's commands: b/e/f T INDEX VALUE, m T usb|ble BYTES
            if ev:
                kind, index, value, injected = ev
                where = ("ble" if index == 1 else "usb") if kind == "m" else "%d" % index
                commands.append("%s %d %s %s%s" % (kind, t, where, value, " inj" if injected else ""))
        elif r.get("module") == "engine":
            out = corelog.parse_out(msg)
            if out:
                want.append(out)
    with tempfile.TemporaryDirectory() as tmp:
        lines = run_sim(sim, commands, os.path.join(tmp, "device.bin"))
    got = [" ".join(line.split()[3:]) for line in lines if line.startswith("M ")]
    inputs = len(commands) - 1 - len(settings)
    if got == want:
        return True, "%d input events, %d MIDI messages agree" % (inputs, len(got))
    k = next((i for i, (a, b) in enumerate(zip(got, want)) if a != b), min(len(got), len(want)))
    return False, "%d input events; first difference at message %d: simulator %s, session %s" % (
        inputs, k, got[k] if k < len(got) else "nothing", want[k] if k < len(want) else "nothing")


# --- command line ----------------------------------------------------------------------------------

def main(argv=None):
    p = argparse.ArgumentParser(prog="smcsim", description="Core simulator scenarios (docs/core.md).")
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="run one scenario")
    r.add_argument("scenario")
    r.add_argument("--session")
    r.add_argument("-v", "--verbose", action="store_true")
    a = sub.add_parser("run-all", help="run every scenario in a directory")
    a.add_argument("dir")
    rp = sub.add_parser("replay", help="replay a session's input through the simulator")
    rp.add_argument("session")
    dv = sub.add_parser("device", help="run scenarios on the device over the command link")
    dv.add_argument("scenarios", nargs="+")
    dv.add_argument("--port", required=True)
    g = dv.add_mutually_exclusive_group(required=True)
    g.add_argument("--dict")
    g.add_argument("--binary")
    dv.add_argument("--session")
    dv.add_argument("--settle-ms", type=int, default=300)
    dv.add_argument("--midi", help="also compare the MIDI the host receives: raw:PATH or coremidi:NAME")
    for q in (r, a, rp, dv):
        q.add_argument("--sim", default=DEFAULT_SIM)
    for q in (r, a):
        q.add_argument("--level", type=int, default=4, help="smclog level for every module (default 4, DEBUG)")
    args = p.parse_args(argv)

    if not os.access(args.sim, os.X_OK):
        sys.exit("smcsim: no simulator at %s (make -C firmware sim)" % args.sim)
    try:
        if args.cmd == "run":
            ok, failures, d = run_scenario(args.scenario, args.sim, args.session, args.level, args.verbose)
            for f in failures:
                print("  FAIL " + f)
            print("smcsim: %s %s (%d steps, %d MIDI messages, %d log records)" % (
                "ok  " if ok else "FAIL", args.scenario, d["steps"], d["messages"], d["records"]))
            return 0 if ok else 1
        if args.cmd == "run-all":
            files = sorted(f for f in os.listdir(args.dir) if f.endswith(".jsonl"))
            if not files:
                sys.exit("smcsim: no scenarios in %s" % args.dir)
            bad = 0
            for name in files:
                ok, failures, d = run_scenario(os.path.join(args.dir, name), args.sim, None, args.level)
                print("%s %s (%d steps, %d MIDI messages)" % ("ok  " if ok else "FAIL", name, d["steps"], d["messages"]))
                for f in failures:
                    print("     " + f)
                bad += not ok
            print("smcsim: %d scenarios, %s" % (len(files), "all ok" if not bad else "%d FAILED" % bad))
            return 1 if bad else 0
        if args.cmd == "device":
            sys.path.insert(0, HERE)
            import device
            return device.main(args)
        ok, detail = replay(args.session, args.sim)
        print("smcsim: replay %s: %s" % ("ok" if ok else "MISMATCH", detail))
        return 0 if ok else 1
    except (ScenarioError, smclog.SmclogError, OSError) as e:
        sys.exit("smcsim: %s" % e)


if __name__ == "__main__":
    sys.exit(main())

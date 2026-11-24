#!/usr/bin/env python3
"""make e2e: the editor's configurations on the mixer (docs/integration.md, T-3, I-4).

For each configuration (config/factory.json, config/test.json and the editor's generated
tools/editortest/vectors/gen-N.json):

1. push: the configuration goes to the mixer over the configuration protocol (SysEx on the
   "SMC-Mixer Config" MIDI port by default, --transport cdc for the USB CDC port); CFG_INFO must
   show it running with the supplied ID and mixer-assigned revision; pull confirms both and content.
2. sends: the probe plan of the editor's contract tests (tools/editortest/vectors.py), for a few
   (preset, page, Shift) blocks, is injected into the mixer's core in real time (INPUT_INJECT over
   the CDC link), presets and pages selected with the panel's own buttons (Shift + a double arrow,
   Shift + an arrow). The simulator (build/host/smcsim --config) gets the same input, and the MIDI
   the host receives on the "SMC-Mixer" port must equal the simulator's, message for message. The
   simulator's messages equal the editor's prediction (midi.js) for every probe of the whole plan:
   that is T-1 (make quick). The host's MIDI must also equal what the mixer logged it sent (the
   USB MIDI path). A real input during the run (a fader that jitters) is reported: it changes the
   core's state, so the comparison after it does not hold.
3. Live edits and saving (I-6, P-1, Q-26; --edit FROM:TO, factory:gen-2 by default, none to
   skip): FROM is pushed (a restart: the state as the simulator's), then turned into TO by live
   edits (CFG_SET, codec.edits(): no restart); CFG_INFO must show the advanced revision and the ID kept,
   the storage unsaved, a pull must give TO's binary form byte for byte, and the probe plan runs
   on it against the simulator's TO as in 2. Then SAVE (stored in flash: storage clean, the saved
   generation one more), one more edit (unsaved), REVERT: the saved TO again (source stored, a pull
   byte for byte).
4. The panel (I-7, P-2, P-3, P-6; --no-panel to skip), over the configuration's transport: EVENTS
   on gives the panel's state at once; CFG_SELECT switches the mixer (an EVENT shows it); touches
   injected over the same link (an encoder, a bottom-row button) are counted and named in the
   EVENTs by the binary form's address; EVENTS off stops them; CFG_STATE agrees with the last EVENT.
5. Before any test, back up the running and stored configurations separately (an existing saved,
   stable baseline is required). At the end restore both, even if a phase saved and then failed:
   original unsaved edits stay unsaved, and the mixer still starts with its original stored copy.
   Saved content is checked through REVERT and read-back; restoration assigns new revisions and
   may advance the saved generation.

  python3 -I tools/e2e/e2e.py --port /dev/cu.usbmodemNN [--transport sysex|cdc]
        [--configs factory,test,gen-1,gen-2] [--blocks 0:0,0:0s,1:2,3:3s] [--binary build/app-ble/app.elf]
        [--config-port "SMC-Mixer Config"] [--midi-in SMC-Mixer] [--ble PORT] [--edit FROM:TO|none]
        [--no-panel]

Over BLE MIDI: --ble names the mixer's Bluetooth MIDI port (connected in the OS first: Audio MIDI
Setup on macOS) for both the configuration and the MIDI in; the injection and the device's log stay
on the CDC. BLE's queue keeps only a continuous control's newest value under load (FR-IO-08;
docs/ble.md, "Last value wins"), so there the MIDI the mixer logged must equal the simulator's, and
the host's must be the logged messages with only such older values left out. The MIDI ports are
tools/hostmidi's (CoreMIDI, ALSA, WinMM).

A block P:G is preset P+1, page G+1 without Shift; P:Gs with Shift. A report goes to
logs/e2e/DATE-TIME/ (report.txt, the device's log, the host's MIDI), which stays local (SPEC C-7).
Exit 0 when every check passed. Talks to the mixer: only with the owner's go-ahead (AGENTS.md).
Park the faders at an end before a run (core.md, "Scenarios on the device").

Standard library only; needs build/host/smcsim (make -C firmware sim).
"""
import argparse
import datetime
import json
import os
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
for d in ("tools/editortest", "tools/smcconf", "tools/smclog", "tools/smcsim", "tools/hostmidi"):
    sys.path.insert(0, os.path.join(ROOT, d))
import codec  # noqa: E402
import corelog  # noqa: E402
import devlink  # noqa: E402
import hostmidi  # noqa: E402
import smclog  # noqa: E402
import vectors  # noqa: E402

SIM = os.path.join(ROOT, "build", "host", "smcsim")
CONFIGS = {"factory": "config/factory.json", "test": "config/test.json",
           "gen-1": "tools/editortest/vectors/gen-1.json", "gen-2": "tools/editortest/vectors/gen-2.json"}
CONFIG_PORT = "SMC-Mixer Config"  # the defaults: USB MIDI
PERF_PORT = "SMC-Mixer"
E2E_ID = bytes.fromhex("e2e0e2e0e2e04e2e8e2ee2e0e2e0e2e0")
LEAD_S = 0.2     # from the start to the plan's first time
SETTLE_S = 0.6   # after the plan's end
LOG_LEVELS = {"input": 4, "engine": 4}  # DEBUG: the core's input records and its MIDI out


def backup_configuration(dev, out_dir):
    """Keep the running and stored copies separate, including unsaved edits at the start.

    Without an existing saved baseline the protocol cannot undo a test's first Save: it has no
    command to remove that saved slot. Refuse before changing the device rather than hide that.
    """
    info = dev.info()
    if not info["generation"]:
        raise devlink.DeviceError("save a baseline configuration before running e2e; no saved copy to restore")
    if info["store"] not in ("clean", "unsaved"):
        raise devlink.DeviceError("wait for a stable configuration before e2e (storage %s)" % info["store"])
    original = dev.pull(info["chunk"])
    dev.abort()
    with open(os.path.join(out_dir, "before.bin"), "wb") as f:
        f.write(original)
    stored = original
    if info["store"] != "clean":
        try:
            dev.revert()
            stored = dev.pull(info["chunk"])
        finally:
            dev.abort()
            dev.push(original, chunk=min(info["chunk"], 128))
            dev.abort()
    with open(os.path.join(out_dir, "stored-before.bin"), "wb") as f:
        f.write(stored)
    return {"info": info, "running": original, "stored": stored}


def same_configuration(back, original):
    """Compare canonical data and ID; replaying a backup cannot restore its revision."""
    if back == original:
        return True
    current = codec.decode(back)
    return current == dict(codec.decode(original), revision=current["revision"])


def restore_configuration(dev, backup):
    """Restore saved content if any phase saved, even if that phase then failed before returning."""
    info = backup["info"]
    changed = dev.info()["generation"] != info["generation"]
    chunk = min(info["chunk"], 128)
    try:
        if changed:
            dev.abort()
            dev.push(backup["stored"], chunk=chunk)
            dev.abort()
            dev.save()
            dev.revert()
            if not same_configuration(dev.pull(info["chunk"]), backup["stored"]):
                raise devlink.DeviceError("the original stored configuration content did not read back identically")
    finally:
        dev.abort()
        dev.push(backup["running"], chunk=chunk)
        dev.abort()
    if info["store"] == "clean":
        dev.save()
    back = dev.pull(info["chunk"])
    dev.abort()
    after = dev.info()
    if not same_configuration(back, backup["running"]) or after["store"] != info["store"]:
        raise devlink.DeviceError("the original running configuration or saving state was not restored")
    return after


def parse_blocks(text):
    out = []
    for b in text.split(","):
        shift = b.endswith("s")
        p, g = b.rstrip("s").split(":")
        out.append((int(p), int(g), shift))
    return out


def sim_run(doc, lines):
    """The simulator's output lines on doc for the plan's lines (nav markers left out)."""
    with tempfile.TemporaryDirectory(prefix="e2e-") as d:
        cfg = os.path.join(d, "config.bin")
        with open(cfg, "wb") as f:
            f.write(codec.encode(codec.from_json(doc)))
        p = subprocess.run([SIM, "--config", cfg], input="\n".join(l for l in lines if not l.startswith("nav "))
                           + "\n", capture_output=True, text=True, timeout=300)
    if p.returncode:
        raise SystemExit("smcsim failed: %s" % p.stderr)
    out = p.stdout.splitlines()
    bad = [l for l in out if l.startswith("W ")]
    if bad:
        raise SystemExit("smcsim did not understand: %s" % bad[0])
    return out


def device_plan(doc, blocks):
    """(lines, expected messages [(step, hex)], the number of Shift taps per block): the plan with
    the panel's navigation, the taps found by running it in the simulator first."""
    nav = {}
    for _ in range(3):
        lines, probes = vectors.plan(doc, blocks=set(blocks), nav=nav)
        out = sim_run(doc, lines)
        states = [[int(x) for x in l.split()[1:]] for l in out if l.startswith("S ")]
        # the state after each block's navigation: the first "state" after its nav marker
        k, after_nav, changed = 0, [], False
        for i, l in enumerate(lines):
            if l == "state":
                k += 1
            elif l.startswith("nav "):
                p, g = (int(x) for x in l.split()[1:])
                j = lines.index("state", i)
                after_nav.append(((p, g), states[sum(1 for x in lines[:j] if x == "state")]))
        for (p, g), st in after_nav:
            if st[:2] != [p, g]:
                raise SystemExit("e2e: the navigation did not reach preset %d page %d in the simulator: %s"
                                 % (p + 1, g + 1, st))
            if len(st) > 2 and st[2]:
                nav[(p, g)] = nav.get((p, g), 0) + 1
                changed = True
        if not changed:
            break
    else:
        raise SystemExit("e2e: Shift stays on after the navigation")
    expected, step = [], 0
    for l in out:
        w = l.split()
        if w[0] == "#":
            step = int(w[1])
        elif w[0] == "M":
            expected.append((step, " ".join(x.upper() for x in w[3:])))
    return lines, expected, probes


def probe_of(probes, step):
    for pr in probes:
        steps = pr["phases"].values()
        if min(steps) <= step <= max(steps) + 1:
            phase = max((s, n) for n, s in pr["phases"].items() if s <= step)[1] if step >= min(steps) else "?"
            return "preset %d page %d %s%s (%s)" % (pr["p"] + 1, pr["g"] + 1, pr["id"], " + Shift" if pr["shift"] else "",
                                                    phase)
    return "navigation"


def inject_event(w):
    kind, idx, v = w[0], int(w[2]), int(w[3])
    if kind == "b":
        return bytes([1, idx, v, 0, 0])
    return bytes([{"e": 2, "f": 3}[kind], idx]) + int(v).to_bytes(2, "little", signed=True) + b"\0"


class Log:
    """The CDC link: injection, and the device's log while the plan runs."""

    def __init__(self, port, dictionary):
        self.fd = smclog.open_serial(port)
        self.records = []
        self.link = smclog.Link(self.fd, smclog.Decoder(dictionary), on_record=self.records.append)
        self.dictionary = dictionary
        info = self.ask("info")
        if dictionary.get("build_id") and info["build_id"] != dictionary["build_id"]:
            raise SystemExit("e2e: the mixer runs build %s, the dictionary is %s" % (info["build_id"],
                                                                                    dictionary["build_id"]))
        self.levels = self.ask("levels").get("levels", [])
        self.ask("subscribe")
        for module, level in LOG_LEVELS.items():
            self.ask("level", bytes([smclog.module_index(module, dictionary), level]))

    def ask(self, name, payload=b""):
        r = self.link.call(smclog.CMDS[name], payload)
        if r["status"] != "ok":
            raise SystemExit("e2e: the mixer refused %s: %s" % (name, r["status"]))
        return r

    def pump_until(self, t_mono):
        while True:
            left = t_mono - time.monotonic()
            recs, _ = self.link.read(max(0.0, min(0.002, left)))
            self.records += recs
            if left <= 0:
                return

    def close(self):
        try:
            self.ask("inject", b"\x00")
            for i, level in enumerate(self.levels):
                self.ask("level", bytes([i, level]))
            self.link.send(smclog.CMDS["unsubscribe"])
        except (SystemExit, smclog.SmclogError, OSError):
            pass
        os.close(self.fd)


def play(log, lines):
    """Injects the plan's input at its times; returns how far the injections were late at most (ms)."""
    log.ask("inject", b"\x01")
    timed = [(int(l.split()[1]), l.split()) for l in lines if l[:2] in ("b ", "f ", "e ", "t ")]
    t_first = timed[0][0]
    t0 = time.monotonic() + LEAD_S
    late, i = 0.0, 0
    while i < len(timed):
        t, w = timed[i]
        group = []
        while i < len(timed) and timed[i][0] == t:
            if timed[i][1][0] != "t":
                group.append(inject_event(timed[i][1]))
            i += 1
        at = t0 + (t - t_first) / 1e6
        log.pump_until(at)
        late = max(late, (time.monotonic() - at) * 1000)
        for k in range(0, len(group), 32):
            r = log.ask("inject", b"\x02" + b"".join(group[k:k + 32]))
            if r.get("inject") != "ok":
                raise SystemExit("e2e: the mixer refused an injection: %s" % r)
    log.pump_until(time.monotonic() + SETTLE_S)
    return late


def merged_away(logged, host):
    """BLE's last value wins: None when host is logged with only older values of continuous
    controls left out (a CC, pitch bend or channel pressure whose same status and controller the
    host received later), else what is wrong."""
    i, dropped = 0, []
    for k, m in enumerate(logged):
        if i < len(host) and host[i] == m:
            i += 1
        else:
            dropped.append(k)
    if i != len(host):
        return "the host received %s at message %d, which the mixer did not send there" % (host[i], i)
    kept = set(range(len(logged))) - set(dropped)
    for k in dropped:
        m = logged[k]
        kind = int(m[:2], 16) >> 4
        key = m[:5] if kind == 0xB else m[:2]
        if kind not in (0xB, 0xD, 0xE) or not any(j > k and logged[j].startswith(key) for j in kept):
            return "message %d (%s) lost, and no newer value of it came" % (k, m)
    return None


def real_inputs(records):
    """Input records the core took that were not injected (a jittering fader)."""
    out = []
    for r in records:
        ev = corelog.parse_input(r.get("msg", "")) if r.get("kind") == "log" and r.get("module") == "input" else None
        if ev and not ev[3]:
            out.append(r["msg"])
    return out


def open_conf(a):
    return devlink.Device(port=a.port if a.transport == "cdc" else None,
                          midi=a.config_port if a.transport == "sysex" else None, timeout=1.0)


def edit_phase(a, info, check, sends):
    """Step 3: live edits from one configuration to another, the probe plan on the result, SAVE,
    another edit, REVERT. Returns True once SAVE was sent (the flash then holds another
    configuration than at the start)."""
    src, dst = a.edit.split(":")
    docs = {}
    for name in (src, dst):
        with open(os.path.join(ROOT, CONFIGS[name]), encoding="utf-8") as f:
            docs[name] = json.load(f)
    m_src, m_dst = (codec.from_json(json.loads(json.dumps(docs[n]))) for n in (src, dst))
    m_src["id"], m_src["revision"] = E2E_ID, 2000
    m_dst["id"], m_dst["revision"] = E2E_ID, 2001
    want_store = "waiting" if docs[dst].get("global", {}).get("save") == "auto" else "unsaved"
    edits = codec.edits(m_src, m_dst)
    chunk = min(info["chunk"], 128)
    with open_conf(a) as dev:
        previous = dev.info()["revision"]
        next_revision = (previous + 1) & 0xFFFFFFFF
        # Use a distinct legacy input so the upload tells us which semantics the device uses.
        m_src["revision"] = 2000 if next_revision != 2000 else 1999
        uploaded = dev.push(codec.encode(m_src), chunk=chunk)["revision"]
        if uploaded not in (next_revision, m_src["revision"]):
            raise devlink.DeviceError("unexpected running revision after the edit baseline upload")
        owns_revision = uploaded == next_revision
        t = time.monotonic()
        after = dev.edit(edits, 2001)
        took = time.monotonic() - t
        want_rev = (uploaded + len(edits)) & 0xFFFFFFFF if owns_revision else 2001
        check("edit %s to %s: %d live edits (CFG_SET, %d bytes) over %s, running" % (
            src, dst, len(edits), sum(len(r) for _, r in edits), a.transport),
            (after["source"], after["revision"], after["id"], after["store"]) == ("received", want_rev, E2E_ID, want_store),
            "%s, revision %d, storage %s, %.2f s" % (after["source"], after["revision"], after["store"], took))
        back = dev.pull(info["chunk"])
        dev.abort()
        check("edit: pull gives %s with its running revision" % dst,
              back == codec.encode(dict(m_dst, revision=want_rev)), "%d bytes" % len(back))
    sends("%s-edited" % dst, docs[dst])
    with open_conf(a) as dev:
        before = dev.info()
        saved = True
        after = dev.save()
        check("save: the mixer starts with %s now" % dst,
              after["store"] == "clean" and after["generation"] == before["generation"] + 1
              and after["revision"] == before["revision"],
              "storage %s, generation %d to %d" % (after["store"], before["generation"], after["generation"]))
        m_more = codec.from_json(json.loads(json.dumps(docs[dst])))
        m_more["id"], m_more["revision"] = E2E_ID, 2002
        enc = m_more["presets"][0]["pages"][0]["strips"][0]["encoder"]["normal"]
        enc["number"] = (enc["number"] + 1) % 128
        more = codec.edits(m_dst, m_more)
        after = dev.edit(more, 2002)
        more_rev = (want_rev + len(more)) & 0xFFFFFFFF if owns_revision else 2002
        check("edit after the save: unsaved", after["store"] == want_store and after["revision"] == more_rev,
              "storage %s, revision %d" % (after["store"], after["revision"]))
        after = dev.revert()
        back = dev.pull(info["chunk"])
        dev.abort()
        revert_rev = (more_rev + 1) & 0xFFFFFFFF if owns_revision else want_rev
        check("revert: the saved %s content with the running revision" % dst,
              (after["source"], after["revision"], after["store"]) == ("stored", revert_rev, "clean")
              and back == codec.encode(dict(m_dst, revision=revert_rev)),
              "%s, revision %d, storage %s, %d bytes" % (after["source"], after["revision"], after["store"], len(back)))
    return saved


def panel_phase(a, check):
    """Step 4: the panel's state, its EVENTs, a selection and touches, and what the mixer sent with
    the control (P-5), over the configuration's transport."""
    def until(dev, want, timeout=2.0):
        """EVENTs until one satisfies want; (that one, all seen, seconds) or (None, ...)."""
        seen, t0 = [], time.monotonic()
        while time.monotonic() - t0 < timeout:
            for count, st in dev.events(0.05):
                seen.append((count, st))
                if want(st):
                    return st, seen, time.monotonic() - t0
        return None, seen, time.monotonic() - t0

    def inject(dev, payload):
        r = dev.link.call(smclog.CMDS["inject"], payload, timeout=dev.timeout)
        if r["status"] != "ok" or r.get("inject") != "ok":
            raise devlink.DeviceError("inject: %s %s" % (r["status"], r.get("inject")))

    with open_conf(a) as dev:
        st0 = dev.state()
        dev.watch(True, sent=True)
        first, seen, _ = until(dev, lambda st: True, 1.0)
        check("panel: EVENTS on over %s, the state at once" % a.transport,
              first is not None and seen[0][0] == 0 and (first["preset"], first["page"]) == (st0["preset"], st0["page"]),
              "preset %d page %d, %d touches" % (first["preset"] + 1, first["page"] + 1, first["touches"]) if first else "none")
        dev.select(2, 1)
        got, seen, took = until(dev, lambda st: (st["preset"], st["page"]) == (2, 1))
        check("panel: SELECT preset 3 page 2, the EVENT shows it", got is not None, "%.0f ms" % (took * 1000))
        touches = (got or st0)["touches"]
        inject(dev, b"\x01")
        try:
            inject(dev, b"\x02" + inject_event(("e", 0, 5, 1)))  # encoder 6, a click and back
            inject(dev, b"\x02" + inject_event(("e", 0, 5, -1)))
            enc, seen, took = until(dev, lambda st: st["touches"] == (touches + 2) % 256)
            check("panel: two encoder touches counted and named (preset 3, page 2, control 13)",
                  enc is not None and enc["touched"] == {"preset": 2, "page": 1, "control": 13, "layer": 0},
                  "%s, %.0f ms" % (enc and enc["touched"], took * 1000))
            inject(dev, b"\x02" + inject_event(("b", 0, 32 + 3, 1)))  # the row's Rew: a press, a release
            inject(dev, b"\x02" + inject_event(("b", 0, 32 + 3, 0)))
            row, seen, took = until(dev, lambda st: st["touches"] == (touches + 3) % 256)
            check("panel: a bottom-row button named by the row's page (255) and its index",
                  row is not None and row["touched"] == {"preset": 2, "page": devlink.ROW_PAGE, "control": 3, "layer": 0},
                  "%s" % (row and row["touched"]))
            # P-5: what those four events sent, each message with its control, in order. How many
            # each sends is the configuration's (an encoder with a divider may send none for a click
            # and back, a trigger nothing on release); the row's press sends in every one e2e runs.
            enc_src = {"preset": 2, "page": 1, "control": 13, "layer": 0}
            rew_src = {"preset": 2, "page": devlink.ROW_PAGE, "control": 3, "layer": 0}
            lost, sent, quiet = 0, [], time.monotonic() + 2.0
            while time.monotonic() < quiet:
                n, e = dev.sent(0.1)
                lost, sent = lost + n, sent + e
                if e and any(x["source"] == rew_src for x in sent):
                    quiet = time.monotonic() + 0.5  # until half a second passes with nothing new
            srcs = [x["source"] for x in sent]
            k = srcs.index(rew_src) if rew_src in srcs else len(srcs)
            ordered = srcs[:k] == [enc_src] * k and srcs[k:] == [rew_src] * (len(srcs) - k) and k < len(srcs)
            consecutive = all(b["n"] == (a["n"] + 1) & 0xFFFFFFFF for a, b in zip(sent, sent[1:]))
            again = dev.sent_since(sent[0]["n"])[1] if sent else []
            name = {json.dumps(enc_src, sort_keys=True): "encoder 6", json.dumps(rew_src, sort_keys=True): "Rew"}
            check("panel: SENT names the control of every message: the encoder's, then the row's (P-5)",
                  not lost and ordered and consecutive and again == sent[:16],
                  "%s; %d lost" % (", ".join("%s from %s" % (x["msg"].hex(), name.get(json.dumps(x["source"], sort_keys=True), x["source"]))
                                             for x in sent) or "none", lost))
        finally:
            inject(dev, b"\x00")
        dev.select(0, 0)
        back, seen, _ = until(dev, lambda st: (st["preset"], st["page"]) == (0, 0))
        dev.watch(False)
        late = dev.events(0.3)
        st1 = dev.state()
        late_sent = dev.sent(0.1)[1]
        check("panel: back on preset 1 page 1; EVENTS off stops them; CFG_STATE as the last EVENT",
              back is not None and not late and not late_sent and st1 == back,
              "%d after off" % len(late))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", required=True, help="the mixer's USB CDC port, or auto (found by its USB IDs)")
    ap.add_argument("--transport", choices=("sysex", "cdc"), default="sysex")
    ap.add_argument("--configs", default="factory,test,gen-1,gen-2")
    ap.add_argument("--blocks", default="0:0,0:0s,1:2,3:3s")
    ap.add_argument("--binary", default=os.path.join(ROOT, "build", "app-ble", "app.elf"))
    ap.add_argument("--config-port", default=CONFIG_PORT, help="the MIDI port of the configuration protocol")
    ap.add_argument("--midi-in", default=PERF_PORT, help="the MIDI port the mixer's controls are received on")
    ap.add_argument("--ble", metavar="PORT", help="over BLE MIDI: the mixer's Bluetooth MIDI port for both")
    ap.add_argument("--edit", default="factory:gen-2", help="live edits from one configuration to another, "
                    "then save and revert (none: skip)")
    ap.add_argument("--no-panel", action="store_true", help="skip the panel's state, events and selection")
    a = ap.parse_args(argv)
    if a.ble:
        a.config_port = a.midi_in = a.ble
    blocks = parse_blocks(a.blocks)
    out_dir = os.path.join(ROOT, "logs", "e2e", datetime.datetime.now().strftime("%Y-%m-%d-%H%M%S"))
    os.makedirs(out_dir)
    report = []

    def check(name, ok, detail=""):
        line = "%s %s%s" % ("PASS" if ok else "FAIL", name, ": " + detail if detail else "")
        print(line, flush=True)
        report.append(line)
        return ok

    dictionary = smclog.make_dictionary(a.binary)

    def sends(name, doc):
        """The probe plan on the configuration the mixer runs (doc) against the simulator's."""
        lines, expected, probes = device_plan(doc, blocks)
        log = Log(a.port, dictionary)
        try:
            first = len(log.records)
            host.mark()
            t = time.monotonic()
            late = play(log, lines)
            took = time.monotonic() - t
            got = [(tt, hostmidi.hex_of(msg)) for tt, msg in host.take()]
            records = log.records[first:]
        finally:
            log.close()
        with open(os.path.join(out_dir, "%s-device.jsonl" % name), "w", encoding="utf-8") as f:
            for r in records:
                f.write(json.dumps(r) + "\n")
        with open(os.path.join(out_dir, "%s-midi.txt" % name), "w", encoding="utf-8") as f:
            for tt, h in got:
                f.write("%d %s\n" % (tt, h))
        logged = [corelog.parse_out(r.get("msg", "")) for r in records
                  if r.get("kind") == "log" and r.get("module") == "engine"]
        logged = [o for o in logged if o]
        want = [h for _, h in expected]
        have = logged if a.ble else [h for _, h in got]  # BLE: the core's, the merge below
        k = next((i for i, (x, y) in enumerate(zip(want, have)) if x != y), min(len(want), len(have)))
        detail = "%d messages, %d probes, %.0f s, injections at most %.1f ms late" % (
            len(want), len(probes), took, late)
        if want != have:
            detail = "at message %d (%s): simulator %s, mixer %s; %d expected, %d received" % (
                k, probe_of(probes, expected[k][0]) if k < len(expected) else "after the end",
                want[k] if k < len(want) else "nothing", have[k] if k < len(have) else "nothing",
                len(want), len(have))
        check("%s: the MIDI the mixer %s equals the simulator's (blocks %s)" % (
            name, "logged it sent" if a.ble else "sends", a.blocks), want == have, detail)
        heard = [h for _, h in got]
        if a.ble:
            wrong = merged_away(logged, heard)
            check("%s: the host received what the mixer logged it sent, only older values of continuous "
                  "controls merged away (%s)" % (name, a.midi_in), wrong is None,
                  wrong or "%d logged, %d received, %d merged" % (len(logged), len(heard), len(logged) - len(heard)))
        else:
            check("%s: the host received every message the mixer logged it sent, unchanged (%s)" % (name, a.midi_in),
                  logged == heard, "%d logged, %d received" % (len(logged), len(heard)))
        real = real_inputs(records)
        check("%s: no real input during the run" % name, not real, "; ".join(real[:3]))

    with open_conf(a) as dev:
        info = dev.info()
        dev.check_compatible(info, codec.S.binary_version, codec.S.version)
        backup = backup_configuration(dev, out_dir)
        info, original = backup["info"], backup["running"]
    check("start: the mixer answers over %s, its configuration pulled (%d bytes, %s)" % (
        a.transport, len(original), info["source"]), True)
    host = hostmidi.open_port(a.midi_in).capture()
    via = "%s, MIDI in on %r" % (a.transport if a.transport == "cdc" else "sysex on %r" % a.config_port, a.midi_in)
    check("ports: configuration over %s" % via, True)
    try:
        for n, name in enumerate(a.configs.split(",")):
            with open(os.path.join(ROOT, CONFIGS[name]), encoding="utf-8") as f:
                doc = json.load(f)
            m = codec.from_json(json.loads(json.dumps(doc)))
            m["id"], m["revision"] = E2E_ID, 1000 + n
            blob = codec.encode(m)
            with open_conf(a) as dev:
                previous = dev.info()["revision"]
                after = dev.push(blob, chunk=min(info["chunk"], 128))
                check("%s: push over %s, running" % (name, a.transport),
                      after["source"] == "received" and after["id"] == E2E_ID and
                      after["revision"] in ((previous + 1) & 0xFFFFFFFF, m["revision"]),
                      "%s, revision %d" % (after["source"], after["revision"]))
                back = dev.pull(info["chunk"])
                dev.abort()
                check("%s: pull gives it back with the running revision" % name,
                      back == codec.encode(dict(m, revision=after["revision"])), "%d bytes" % len(back))
            sends(name, doc)
        if a.edit != "none":
            edit_phase(a, info, check, sends)
        if not a.no_panel:
            panel_phase(a, check)
    finally:
        try:  # both original copies go back, even if a test saved and then raised
            with open_conf(a) as dev:
                after = restore_configuration(dev, backup)
            check("end: original running and stored configurations restored and verified",
                  after["source"] == "received" and after["store"] == info["store"],
                  "%s, revision %d, storage %s, generation %d" % (
                      after["source"], after["revision"], after["store"], after["generation"]))
        finally:
            host.close()
            with open(os.path.join(out_dir, "report.txt"), "w", encoding="utf-8") as f:
                f.write("\n".join(report) + "\n")
    failed = sum(1 for l in report if l.startswith("FAIL"))
    print("e2e: %d checks, %d failed; %s" % (len(report), failed, os.path.relpath(out_dir, ROOT)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

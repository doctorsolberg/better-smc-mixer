#!/usr/bin/env python3
"""The shared test vectors of the editor and the firmware (docs/integration.md, T-1; SPEC A-20).

The firmware's side writes them, the editor's side reads them (tools/editortest/run.mjs, in Node in
the build image): files in tools/editortest/vectors/, kept in the repository so a change of either
side shows up as a diff of the vectors and fails the other side's test.

- sends-NAME.json: every control of every page and layer of a configuration (config/test.json,
  config/factory.json, the generated gen-N.json) touched in the core's simulator
  (build/host/smcsim --config, the binary form): the inputs, and per phase the messages the core
  sent and the preset, page and Shift it reports. One probe a line.
- gen-N.json: configurations the editor's edit code writes (run.mjs writes them again and must
  get the same bytes); here they must be valid and canonical for smcconf.
- faults.json: broken files, each with the field smcconf's codec names for it; the editor must
  name the same field.
- binary.json: the binary form smcconf's codec writes for each configuration above (as it is, and
  with an ID and a revision; read back it is the file without its names, which the binary does not
  carry), and broken binaries it refuses (and an unknown optional record it skips); the editor's
  codec (app/editor/js/cfgbin.js) must write the same bytes and read the same files (the owner's
  decision of 2026-11-13: a codec in the editor).
- install.json: installation plans of tools/image/installplan.py on synthetic flashes; the editor's
  js/installplan.js (the browser installer's plan) must give the same report, writes and images.
- edits.json: the live edits (CFG_SET records, P-1) smcconf's codec makes between two
  configurations: whole files of the list above, and small changes of the factory file (a
  "set" list of [path, value]; value null leaves the key out). The editor's Codec.edits() must
  make the same [preset, records] list byte for byte.

  python3 -I tools/editortest/vectors.py            check: the vectors are what the firmware
                                                    gives now (exit 1 and the differences if not)
  python3 -I tools/editortest/vectors.py --write    write them again (after a change of the core,
                                                    the codec, the schema or the probes)

Standard library only; needs build/host/smcsim (make -C firmware sim).
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "smcconf"))
import codec  # noqa: E402

HERE = os.path.join(ROOT, "tools", "editortest")
VECTORS = os.path.join(HERE, "vectors")
SIM = os.path.join(ROOT, "build", "host", "smcsim")
CONFIGS = [("test", os.path.join(ROOT, "config", "test.json")),
           ("factory", os.path.join(ROOT, "config", "factory.json"))]
FADER_MAX = 16383
BUTTON_INDEX = {"m": 0, "s": 1, "r": 2, "select": 3}
ROW_BASE = 32
SHIFT = 44

with open(os.path.join(ROOT, "config", "schema.json"), encoding="utf-8") as _f:
    SCHEMA = json.load(_f)
ROW = SCHEMA["row"]["buttons"]
STRIP = SCHEMA["strip"]["controls"]
DEVICE = SCHEMA["device"]


def generated():
    """The generated configurations kept with the vectors, by name."""
    return sorted((os.path.splitext(f)[0], os.path.join(VECTORS, f)) for f in os.listdir(VECTORS)
                  if f.startswith("gen-") and f.endswith(".json"))


def control_ids():
    """A page's controls in the editor's order (Config.ids): strips, then the bottom row."""
    ids = []
    for s in range(DEVICE["strips"]):
        ids += ["s%d.%s" % (s, c["json"]) for c in STRIP]
    return ids + ["row." + b["json"] for b in ROW]


def kind_of(cid):
    if cid.startswith("row."):
        return "button"
    key = cid.split(".")[1]
    return next(c["kind"] for c in STRIP if c["json"] == key)


def is_nav(cid):
    return cid.startswith("row.") and any(b["json"] == cid[4:] and b.get("nav") for b in ROW)


def index_of(cid):
    if cid.startswith("row."):
        return ROW_BASE + [b["json"] for b in ROW].index(cid[4:])
    s, key = int(cid[1:cid.index(".")]), cid.split(".")[1]
    return s if key in ("fader", "encoder") else 4 * s + BUTTON_INDEX[key]


def setting(doc, fields, obj, key):
    f = next(x for x in SCHEMA[fields]["fields"] if x["json"] == key)
    return obj.get(key, f["default"])


def control(doc, p, g, cid):
    presets = doc.get("presets", [])
    pr = presets[p] if p < len(presets) else {}
    if cid.startswith("row."):
        return pr.get("row", {}).get(cid[4:], {})
    pages = pr.get("pages", [])
    page = pages[g] if g < len(pages) else {}
    strips = page.get("strips", [])
    s = int(cid[1:cid.index(".")])
    return (strips[s] if s < len(strips) else {}).get(cid.split(".")[1], {})


def encoder_clicks(doc, p, g, cid, shift):
    """One click an event, or n for div n (a step needs n clicks)."""
    ctl = control(doc, p, g, cid)
    layer = ctl
    if shift:
        sh = ctl.get("shift")
        if sh is None or sh == "block":
            return 1  # fine (the default Shift layer), or nothing
        if isinstance(sh, dict):
            layer = sh
    return layer.get("n", 2) if layer.get("mode") == "div" else 1


# The navigation buttons (core.md, "Navigation"): Shift + one of these selects a preset or a page.
NAV_PRESET = ["rew", "ff", "bankLeft", "bankRight"]
NAV_PAGE = ["up", "down", "left", "right"]


def nav_index(name):
    return ROW_BASE + [b["json"] for b in ROW].index(name)


def plan(doc, blocks=None, nav=None):
    """The simulator's commands that touch every control, and the probes with their phases' step
    numbers. contract.mjs reads the probes back in this order.

    blocks: only these (preset, page, shift) blocks (tools/e2e). nav: select each block's preset
    and page with the panel's own buttons (Shift + a double arrow, Shift + an arrow) instead of the
    simulator's select, so the device can be given the same input: a dict {(p, g): taps}, the
    Shift taps after that leave Shift off (a latching preset keeps it on)."""
    lines, probes = [], []
    t = [1000]
    step = [0]
    gl = doc.get("global", {})
    rest = setting(doc, "global", gl, "gestureMs") + 50
    hybrid = setting(doc, "global", gl, "hybridMs")

    def now(ms):
        t[0] += round(ms * 1000)
        return t[0]

    def wait(ms):
        for _ in range(-(-ms // 10)):
            lines.append("t %d" % now(10))

    for p in range(DEVICE["presets"]):
        presets = doc.get("presets", [])
        mode = setting(doc, "preset", presets[p] if p < len(presets) else {}, "shiftMode")
        for g in range(DEVICE["pages"]):
            for shift in (False, True):
                if blocks is not None and (p, g, shift) not in blocks:
                    continue
                if nav is None:
                    lines.append("select %d %d %d" % (now(10), p, g))
                else:
                    lines.append("nav %d %d" % (p, g))
                    lines.append("b %d %d 1" % (now(10), SHIFT))
                    for name in (NAV_PRESET[p], NAV_PAGE[g]):
                        lines.append("b %d %d 1" % (now(30), nav_index(name)))
                        lines.append("b %d %d 0" % (now(30), nav_index(name)))
                    lines.append("b %d %d 0" % (now(30), SHIFT))
                    for _ in range(nav.get((p, g), 0)):
                        lines.append("b %d %d 1" % (now(30), SHIFT))
                        lines.append("b %d %d 0" % (now(30), SHIFT))
                    lines.append("state")
                wait(rest)
                for cid in control_ids():
                    if cid.startswith("row.") and g > 0:
                        continue
                    if shift and is_nav(cid):
                        continue
                    kind, ix = kind_of(cid), index_of(cid)
                    pr = {"p": p, "g": g, "id": cid, "shift": shift, "phases": {}}

                    def ph(name):
                        step[0] += 1
                        lines.append("step %d" % step[0])
                        pr["phases"][name] = step[0]

                    ph("enter")
                    if shift:
                        lines.append("b %d %d 1" % (now(10), SHIFT))
                        if mode == "latch":
                            lines.append("b %d %d 0" % (now(30), SHIFT))
                        wait(20)
                    lines.append("state")
                    if kind == "fader":
                        ph("up")
                        for k in range(17):
                            lines.append("f %d %d %d" % (now(10), ix, round(k * FADER_MAX / 16)))
                        ph("down")
                        for k in range(15, -1, -1):
                            lines.append("f %d %d %d" % (now(10), ix, round(k * FADER_MAX / 16)))
                        ph("end")
                        wait(rest)
                    elif kind == "encoder":
                        pr["clicks"] = encoder_clicks(doc, p, g, cid, shift)
                        for d, name in ((1, "cw"), (-1, "ccw")):
                            ph(name)
                            lines.append("e %d %d %d" % (now(10), ix, d * pr["clicks"]))
                            ph(name + "End")
                            wait(rest)
                    else:
                        ph("press")
                        lines.append("b %d %d 1" % (now(10), ix))
                        ph("release")
                        lines.append("b %d %d 0" % (now(60), ix))
                        ph("end")
                        wait(20)
                    ph("exit")
                    if shift:
                        if mode == "latch":
                            lines.append("b %d %d 1" % (now(10), SHIFT))
                        lines.append("b %d %d 0" % (now(30), SHIFT))
                        wait(hybrid + 20 if mode == "hybrid" else 20)
                    lines.append("state")
                    ph("after")
                    probes.append(pr)
    lines.append("quit")
    return lines, probes


def run_sim(doc, lines):
    """The simulator's output split by step: {step: (messages, states)}."""
    with tempfile.TemporaryDirectory(prefix="vectors-") as d:
        cfg = os.path.join(d, "config.bin")
        with open(cfg, "wb") as f:
            f.write(codec.encode(codec.from_json(doc)))
        p = subprocess.run([SIM, "--config", cfg], input="\n".join(lines) + "\n", capture_output=True,
                           text=True, timeout=300)
    if p.returncode:
        raise SystemExit("smcsim failed: %s" % p.stderr)
    out, cur = {}, None
    for line in p.stdout.splitlines():
        w = line.split()
        if w[0] == "#":
            cur = out.setdefault(int(w[1]), ([], []))
        elif w[0] == "M" and cur is not None:
            cur[0].append(" ".join(w[3:]))
        elif w[0] == "S" and cur is not None:
            cur[1].append([int(x) for x in w[1:]])
        elif w[0] == "W":
            raise SystemExit("smcsim did not understand: %s" % line)
    return out


def sends(name, path):
    """The sends vector of one configuration, as text (one probe a line)."""
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    lines, probes = plan(doc)
    sim = run_sim(doc, lines)
    rows = []
    for pr in probes:
        row = {"p": pr["p"], "g": pr["g"], "id": pr["id"], "shift": pr["shift"]}
        if "clicks" in pr:
            row["clicks"] = pr["clicks"]
        midi, states = {}, []
        for phase, step in pr["phases"].items():
            msgs, st = sim.get(step, ([], []))
            if msgs:
                midi[phase] = msgs
            states += st
        row["state"] = states
        row["midi"] = midi
        rows.append(json.dumps(row, separators=(",", ":")))
    rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
    head = json.dumps({"config": rel, "made_by": "tools/editortest/vectors.py", "probes": len(rows),
                       "messages": sum(len(m) for r in rows for m in json.loads(r)["midi"].values())})
    return '{"about": %s,\n"probes": [\n%s\n]}\n' % (head, ",\n".join(rows))


def codec_path(message):
    """The field a codec fault names, in the editor's form of a path."""
    path, _, rest = message.partition(": ")
    path = "" if path == "(top)" else path.lstrip(".")
    if rest.startswith("unknown key '"):
        key = rest[len("unknown key '"):].split("'")[0]
        path = path + "." + key if path else key
    elif rest.startswith("type none takes no other field ("):
        path += "." + rest.split("(")[1].split(",")[0].rstrip(")")
    return path


def faults():
    """faults.json with each file's field as the codec names it now."""
    with open(os.path.join(VECTORS, "faults.json"), encoding="utf-8") as f:
        cases = json.load(f)
    rows = []
    for c in cases:
        try:
            codec.from_json(json.loads(json.dumps(c["file"])))
            raise SystemExit("faults.json: smcconf takes %s" % json.dumps(c["file"]))
        except codec.ConfigError as e:
            rows.append(json.dumps({"file": c["file"], "path": codec_path(str(e))}))
    return "[\n" + ",\n".join(rows) + "\n]\n"


BIN_ID = "5e1f0c2a-0b1d-4e2f-8a3b-4c5d6e7f8091"
BIN_REVISION = 0x89ABCDEF


def without_names(obj):
    """A file object with every name left out (what the binary form gives back)."""
    if isinstance(obj, dict):
        return {k: without_names(v) for k, v in obj.items() if k != "name"}
    if isinstance(obj, list):
        return [without_names(v) for v in obj]
    return obj


def binary():
    """binary.json: one case a line."""
    rows = []

    def case(name, blob, back=None):
        try:
            got = codec.to_json(codec.decode(blob))
        except codec.ConfigError as e:
            return {"name": name, "hex": blob.hex(), "refused": str(e)}
        if back is not None and got != back:
            raise SystemExit("binary.json: %s does not read back as the file without its names" % name)
        return {"name": name, "hex": blob.hex()}

    for name, path in CONFIGS + generated():
        with open(path, encoding="utf-8") as f:
            obj = json.load(f)
        for tagged in (False, True):
            o = dict(obj)
            if tagged:
                o["id"], o["revision"] = BIN_ID, BIN_REVISION
            row = case(name + (" with an ID and a revision" if tagged else ""), codec.encode(codec.from_json(o)),
                       without_names(o))
            row["path"] = os.path.relpath(path, ROOT)
            if tagged:
                row.update(id=BIN_ID, revision=BIN_REVISION)
            rows.append(row)
    with open(CONFIGS[0][1], encoding="utf-8") as f:
        base = codec.encode(codec.from_json(json.load(f)))
    head, body = base[:16], base[16:]

    def rebuild(rest):
        import struct
        import zlib
        return head[:8] + struct.pack("<II", len(rest) - 20, zlib.crc32(rest)) + rest

    with open(CONFIGS[1][1], encoding="utf-8") as f:
        lit = json.load(f)
    lit["global"] = dict(lit.get("global", {}), save="auto", ledLevel=4, ledUnlit=2)
    lit = codec.to_json(codec.from_json(lit))  # in the canonical order
    row = case("factory with the LED light set", codec.encode(codec.from_json(lit)), without_names(lit))
    row["file"] = lit  # no file on disk: the case carries it
    rows.append(row)
    rows.append(case("cut short", base[:20]))
    rows.append(case("a wrong CRC", base[:40] + bytes([base[40] ^ 1]) + base[41:]))
    rows.append(case("a byte too many", base + b"\0"))
    rows.append(case("another binary version", base[:4] + b"\x02" + base[5:]))
    rows.append(case("an unknown record", rebuild(body + bytes([5, 0]))))
    rows.append(case("an unknown optional record (skipped)", rebuild(body[:20] + bytes([0x85, 2, 1, 2]) + body[20:])))
    rows[-1]["same_as"] = rows[0]["name"]
    rows.append(case("a record past the end", rebuild(body + bytes([2, 9, 0]))))
    # the LED light's record (0x81), broken: after the global settings, before the presets
    at = 0
    while body[at] != codec.TAG["preset"]:
        at += 2 + body[at + 1]
    for name, rec in (("the LED light twice", [0x81, 2, 1, 5, 0x81, 2, 1, 5]),
                      ("the LED light with an unknown setting", [0x81, 1, 0x04]),
                      ("an empty LED light record", [0x81, 0]),
                      ("the LED light at level 0", [0x81, 2, 1, 0]),
                      ("an unlit glow of 11", [0x81, 2, 2, 11]),
                      ("the LED light with a byte left over", [0x81, 3, 1, 5, 0])):
        rows.append(case(name, rebuild(body[:at] + bytes(rec) + body[at:])))
        if "refused" not in rows[-1]:
            raise SystemExit("binary.json: %s is not refused" % name)
    return "[\n" + ",\n".join(json.dumps(r, separators=(",", ":")) for r in rows) + "\n]\n"


# Small changes of config/factory.json: (name, [(path, value)]); value None leaves the key out.
EDIT_CHANGES = [
    ("one encoder", [(["presets", 0, "pages", 1, "strips", 0, "encoder"], {"type": "cc7", "number": 77})]),
    ("the save mode", [(["global"], {"save": "auto"})]),
    ("the LED light", [(["global"], {"ledLevel": 3, "ledUnlit": 1})]),
    ("the save mode and the LED light", [(["global"], {"save": "auto", "ledUnlit": 4})]),
    ("a fader left with nothing", [(["presets", 0, "pages", 0, "strips", 3, "fader"], None)]),
    ("a control of another preset and the global settings",
     [(["presets", 2, "pages", 0, "strips", 7, "m"], {"type": "cc7", "number": 9}), (["global"], {"save": "auto"})]),
    ("names only", [(["presets", 1, "name"], "Bass"), (["presets", 0, "pages", 2, "name"], "Drums")]),
]


def changed(doc, changes):
    doc = json.loads(json.dumps(doc))
    for path, value in changes:
        at = doc
        for key in path[:-1]:
            at = at[key]
        if value is None:
            at.pop(path[-1], None)
        else:
            at[path[-1]] = value
    return doc


def edits():
    """edits.json: one case a line."""
    files = dict(CONFIGS + generated())
    docs = {}
    for name, path in files.items():
        with open(path, encoding="utf-8") as f:
            docs[name] = json.load(f)
    rows = []

    def case(row, old, new):
        row["edits"] = [[p, r.hex()] for p, r in codec.edits(codec.from_json(old), codec.from_json(new))]
        rows.append(row)

    names = sorted(docs)
    for a in names:
        for b in names:
            if a == b and a != "factory":
                continue
            case({"name": "%s to %s" % (a, b), "old": os.path.relpath(files[a], ROOT),
                  "new": os.path.relpath(files[b], ROOT)}, docs[a], docs[b])
    for name, changes in EDIT_CHANGES:
        case({"name": name, "old": os.path.relpath(files["factory"], ROOT),
              "set": [[p, v] for p, v in changes]}, docs["factory"], changed(docs["factory"], changes))
        case({"name": name + ", undone", "new": os.path.relpath(files["factory"], ROOT),
              "set": [[p, v] for p, v in changes]}, changed(docs["factory"], changes), docs["factory"])
    return "[\n" + ",\n".join(json.dumps(r, separators=(",", ":")) for r in rows) + "\n]\n"


def sparse(data):
    """[[offset, base64], ...]: the bytes that are not 0xFF (erased flash), in runs."""
    import base64
    runs, i, n = [], 0, len(data)
    while i < n:
        if data[i] == 0xFF:
            i += 1
            continue
        j = i
        while j < n:
            k = j
            while k < n and data[k] != 0xFF:
                k += 1
            gap = k
            while gap < n and data[gap] == 0xFF and gap - k < 64:
                gap += 1
            if gap < n and data[gap] != 0xFF:
                j = gap
                continue
            j = k
            break
        runs.append([i, base64.b64encode(data[i:j]).decode()])
        i = j
    return runs


def install():
    """install.json: installation plans of tools/image/installplan.py on synthetic flashes (the test
    builders of tools/image/test_installplan.py: a stock flash as stored and as the stock firmware
    reads it, the MAC item empty or foreign, already installed, our sectors in use, a VM or a slot
    that cannot work, not a 512 KB image, an earlier installation of ours pointed back at the stock
    area (the update), a run of this installation that stopped, another image's pages). The editor's js/installplan.js must give the same report,
    writes and images (run.mjs)."""
    import base64
    import hashlib
    import random
    sys.path.insert(0, os.path.join(ROOT, "tools", "image"))
    import installplan as IP
    import test_installplan as TIP

    def b64(d):
        return base64.b64encode(d).decode()

    app = TIP.APP
    cfg = random.Random(5).randbytes(0x360)
    tail = random.Random(6).randbytes(0x40)
    stored = TIP.stock_flash()
    busy = bytearray(stored)
    busy[0x45000] = 0
    import flashimg as F
    # an older app.bin of 4 sectors, one more than the new one's: a stale sector to erase
    earlier = bytearray(IP.plan(stored, random.Random(9).randbytes(11200), cfg).after)
    earlier[:F.SECTOR] = F.set_boot_pointer(bytes(earlier[:F.SECTOR]), F.AREA_AT)
    earlier[0x6D000:0x6D040] = bytes(0x40)       # our VM's data
    earlier[0x25000:0x25020] = bytes(range(32))  # a saved configuration
    area = IP.plan(stored, app, cfg).writes[0].data
    stopped = bytearray(stored)
    stopped[IP.SLOT + 0x1000:IP.SLOT + 0x1300] = area[0x1000:0x1300]
    stopped[0x25000:0x25010] = bytes(16)
    foreign = bytearray(stopped)
    foreign[IP.SLOT + 0x1100] ^= 1
    flashes = {"stored": stored, "stock read": TIP.stock_read(stored), "mac empty": TIP.stock_flash(mac="empty"),
               "mac other": TIP.stock_flash(mac="other"), "busy": bytes(busy),
               "installed": IP.plan(stored, app, cfg).after, "earlier": bytes(earlier),
               "stopped": bytes(stopped), "foreign": bytes(foreign)}
    cases = [("stored, with cfg_tool.bin", "stored", {"cfg": True}),
             ("the stock firmware's read, no cfg_tool.bin", "stock read", {}),
             ("the MAC item empty", "mac empty", {"cfg": True}),
             ("the MAC item foreign, a tail", "mac other", {"cfg": True, "tail": True}),
             ("our sectors in use", "busy", {"cfg": True}),
             ("a VM too small", "stored", {"cfg": True, "vm": [0x54000, 0x1000]}),
             ("a slot off the key grid", "stored", {"cfg": True, "at": 0x42000}),
             ("not 512 KB", "stored", {"size": 0x40000}),
             ("already installed", "installed", {"cfg": True}),
             ("an earlier installation of ours (the update)", "earlier", {"cfg": True}),
             ("a run of this installation that stopped", "stopped", {"cfg": True}),
             ("a page that is not this installation's", "foreign", {"cfg": True})]
    rows = []
    for name, key, o in cases:
        flash = flashes[key][:o.get("size", IP.FLASH_SIZE)]
        p = IP.plan(flash, app, cfg if o.get("cfg") else None, tail if o.get("tail") else b"",
                    o.get("at", IP.SLOT), tuple(o["vm"]) if "vm" in o else None)
        sha = lambda d: hashlib.sha256(d).hexdigest() if d else None  # noqa: E731
        rows.append({"name": name, "flash": key, "options": o,
                     "plan": IP.as_json(p),
                     "sha256": {"after": sha(p.after), "phys": sha(p.phys),
                                "writes": [sha(w.data) for w in p.writes]}})
    head = {"app": b64(app), "cfg": b64(cfg), "tail": b64(tail)}
    images = {k: sparse(v) for k, v in flashes.items()}
    return json.dumps({"inputs": head, "flashes": images, "cases": rows}, separators=(",", ":")) + "\n"


def expected():
    """Every vector file's text as the firmware's side gives it now: {file name: text}."""
    out = {"faults.json": faults(), "binary.json": binary(), "edits.json": edits(), "install.json": install()}
    for name, path in CONFIGS + generated():
        out["sends-%s.json" % name] = sends(name, path)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--write", action="store_true", help="write the vectors again")
    a = ap.parse_args(argv)
    if not os.access(SIM, os.X_OK):
        print("vectors.py: no %s: make -C firmware sim" % SIM, file=sys.stderr)
        return 2
    stale = []
    for name, text in expected().items():
        path = os.path.join(VECTORS, name)
        try:
            with open(path, encoding="utf-8") as f:
                old = f.read()
        except FileNotFoundError:
            old = None
        if old == text:
            continue
        if a.write:
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            print("wrote %s" % os.path.relpath(path, ROOT))
        else:
            stale.append(name)
    if stale:
        print("stale vectors: %s; the firmware's side now gives other ones. If the change is meant, "
              "write them again (python3 -I tools/editortest/vectors.py --write), then the editor's "
              "side must pass on them (tools/env/test_editor.sh)" % ", ".join(stale))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

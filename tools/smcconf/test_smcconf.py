"""Tests for tools/smcconf (docs/config.md). Run: python3 -I tools/smcconf/test_smcconf.py

- the schema against the firmware: config/schema.json's device counts are core.h's;
- configuration files: defaults, the preset channel, the Shift layer's rules, errors with paths,
  the canonical form (config/factory.json and config/test.json are in it);
- the published JSON Schema (config/smc-config.schema.json) through a small validator of the
  keywords the generator uses: the example files and random configurations are valid, known faults
  are not, and it never refuses a file the codec accepts;
- agreement with the firmware's codec (firmware/config/cfgbin.c) through build/host/cfgtool (built
  by make -C firmware test): the built-in configurations are config/*.json byte for byte, and on
  damaged and random binary forms both accept and refuse the same ones and re-encode them to the
  same bytes. Without cfgtool these are skipped;
- the smcconf command line.

Random cases come from a fixed seed (SMCCONF_SEED overrides it; a failure prints it).
"""
import copy
import json
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import codec  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
CFGTOOL = os.path.join(ROOT, "build", "host", "cfgtool")
EXAMPLES = {"factory": os.path.join(ROOT, "config", "factory.json"),
            "test": os.path.join(ROOT, "config", "test.json")}
JSON_SCHEMA = os.path.join(ROOT, "config", "smc-config.schema.json")
CORE_H = os.path.join(ROOT, "firmware", "core", "core.h")
SEED = int(os.environ.get("SMCCONF_SEED", "20261108"))
S = codec.S
NONE = codec.NONE


def doc(presets=None, **top):
    """A configuration file's object."""
    d = {"schemaVersion": S.version}
    d.update(top)
    if presets is not None:
        d["presets"] = presets
    return d


def strip_doc(**controls):
    return doc([{"pages": [{"strips": [controls]}]}])


def grow(items, i):
    """items[i], an empty object added for it (and before it) where the list is shorter."""
    while len(items) <= i:
        items.append({})
    return items[i]


def first_strip(m, p=0):
    return m["presets"][p]["pages"][0]["strips"][0]


# --- random configurations -----------------------------------------------------------------------

def random_assign(kind, rnd, dense):
    a = codec.empty_assign()
    msg = rnd.choice(kind.types)
    if msg == NONE:
        return a
    mode = rnd.randrange(len(kind.mode_enum))
    a["msg"], a["mode"] = msg, mode
    for f in S.assign_fields:
        if f.c in ("msg", "mode") or not S.applies(f, kind, msg, mode):
            continue
        if f.members:
            v = 0
            for m in f.members:
                if rnd.random() < dense:
                    v |= rnd.choice(sorted(m["values"].values()))
            a[f.c] = v
        elif f.required or rnd.random() < dense:
            lo, hi = f.min - f.offset, S.field_max(f, msg) - f.offset
            a[f.c] = rnd.choice([lo, hi, rnd.randint(lo, hi)] + [n["value"] for n in f.named])
        else:
            a[f.c] = S.default(f, kind, msg, mode, a)
    return a


def random_control(kind, nav, rnd, p_none, dense):
    ctl = codec.empty_control()
    if rnd.random() >= p_none:
        ctl["normal"] = random_assign(kind, rnd, dense)
    ctl["shift_kind"], ctl["shift"] = codec.default_shift(kind, ctl["normal"])
    if nav or rnd.random() < 0.5:
        return ctl
    k = rnd.choice([codec.INHERIT, codec.BLOCK, codec.OVERRIDE, codec.OVERRIDE])
    if k == codec.OVERRIDE:
        if ctl["normal"]["msg"] != NONE and rnd.random() < 0.6:
            s = dict(ctl["normal"])  # a few fields changed
            other = random_assign(kind, rnd, 1.0)
            if other["msg"] == s["msg"] and other["mode"] == s["mode"]:
                for f in rnd.sample(codec.FIELDS[1:], rnd.randint(1, 3)):
                    s[f] = other[f]
        else:
            s = random_assign(kind, rnd, dense)
        if s["msg"] == NONE:
            k, s = codec.BLOCK, codec.empty_assign()
    else:
        s = codec.empty_assign()
    trial = dict(ctl, shift_kind=k, shift=s)
    try:
        codec.check_control(kind, nav, trial, "")
    except codec.ConfigError:
        return ctl
    return trial


def random_setting(f, rnd):
    e = S.enums.get(f.enum)
    if e is not None:
        return rnd.choice([i for i in range(len(e)) if e.json[i] not in f.exclude])
    return rnd.randint(f.min - f.offset, f.max - f.offset)


def random_model(rnd):
    m = codec.empty_config()
    p_none = rnd.choice([0.2, 0.6, 0.95, 1.0])
    dense = rnd.choice([0.1, 0.5, 1.0])
    if rnd.random() < 0.5:
        m["id"] = bytes(rnd.randrange(256) for _ in range(16))
        m["revision"] = rnd.randrange(1 << 32)
    for f in S.global_fields:
        if rnd.random() < 0.3:
            m["global"][f.c] = random_setting(f, rnd)
    for preset in m["presets"]:
        for f in S.preset_fields:
            if f.c and rnd.random() < 0.3:
                preset[f.c] = random_setting(f, rnd)
        for pg, cid, kind, nav, ctl in list(codec.controls(preset)):
            new = random_control(kind, nav, rnd, p_none, dense)
            if pg == codec.ROW_PAGE:
                preset["row"][cid] = new
            else:
                group, n = divmod(cid, 8)
                preset["pages"][pg]["strips"][n][codec.STRIP_KEYS[group]] = new
    codec.check_model(m)
    return m


# --- the binary form, taken apart and damaged ---------------------------------------------------

def records(blob):
    hsize = blob[5]
    out, off = [], hsize
    while off < len(blob):
        n = 2 + blob[off + 1]
        out.append(bytes(blob[off:off + n]))
        off += n
    return out


def build(head, recs):
    """A binary form from its header and records, with the body length and CRC made right."""
    head = bytearray(head)
    body = b"".join(recs)
    struct.pack_into("<I", head, 8, len(body))
    struct.pack_into("<I", head, 12, zlib.crc32(bytes(head[16:]) + body))
    return bytes(head) + body


def damage(blob, rnd):
    head, recs = bytearray(blob[:36]), records(blob)
    what = rnd.randrange(12)
    if what == 0 and recs:  # a byte of a record's payload
        i = rnd.randrange(len(recs))
        r = bytearray(recs[i])
        if len(r) > 2:
            j = rnd.randrange(2, len(r))
            r[j] = rnd.choice([0, 1, 2, 3, 4, 15, 16, 31, 32, 127, 128, 254, 255, rnd.randrange(256)])
        recs[i] = bytes(r)
    elif what == 1 and recs:
        del recs[rnd.randrange(len(recs))]
    elif what == 2 and recs:
        i = rnd.randrange(len(recs))
        recs.insert(i, recs[i])
    elif what == 3 and len(recs) > 1:
        i = rnd.randrange(len(recs) - 1)
        recs[i], recs[i + 1] = recs[i + 1], recs[i]
    elif what == 4:  # an optional record of a future version
        n = rnd.randrange(8)
        recs.insert(rnd.randrange(len(recs) + 1), bytes([0x80 | rnd.randrange(1, 128), n]) + os.urandom(n))
    elif what == 5:  # an unknown record that may not be skipped
        recs.insert(rnd.randrange(len(recs) + 1), bytes([rnd.randrange(5, 128), 1, 0]))
    elif what == 6 and recs:  # a record's length
        i = rnd.randrange(len(recs))
        r = bytearray(recs[i])
        r[1] = (r[1] + rnd.choice([-1, 1])) & 0xFF
        recs[i] = bytes(r)
    elif what == 7 and recs:  # a field mask bit
        i = rnd.randrange(len(recs))
        r = bytearray(recs[i])
        at = {1: 2, 2: 3, 3: 4, 4: 5}.get(r[0], 2)
        if at < len(r):
            r[at] ^= 1 << rnd.randrange(8)
        recs[i] = bytes(r)
    elif what == 8:  # any byte at all, the CRC not fixed
        b = bytearray(build(head, recs))
        b[rnd.randrange(len(b))] ^= 1 << rnd.randrange(8)
        return bytes(b)
    elif what == 9:  # the header
        field = rnd.choice(["version", "magic", "short", "long"])
        if field == "version":
            head[4] = rnd.choice([0, 2, 255])
        elif field == "magic":
            head[rnd.randrange(4)] ^= 0x20
        elif field == "short":
            return bytes(build(head, recs)[:rnd.randrange(40)])
        else:  # a longer header, as a later version may write: still read
            head[5] = 40
            return build(head + bytes(4), recs)
    elif what == 10 and recs:  # a control moved to another page or id
        i = rnd.randrange(len(recs))
        r = bytearray(recs[i])
        if r[0] in (3, 4) and len(r) > 3:
            r[2 + rnd.randrange(2)] = rnd.choice([0, 3, 4, 10, 11, 40, 47, 48, 254, 255])
        recs[i] = bytes(r)
    else:  # a record cut short
        if recs:
            i = rnd.randrange(len(recs))
            r = bytearray(recs[i])
            if r[1]:
                r = r[:-1]
                r[1] -= 1
            recs[i] = bytes(r)
    return build(head, recs)


def py_result(blob):
    try:
        return "ok " + codec.encode(codec.decode(blob)).hex()
    except codec.ConfigError:
        return "error"


def c_results(blobs):
    with tempfile.TemporaryDirectory() as d:
        src, dst = os.path.join(d, "in.bin"), os.path.join(d, "out.txt")
        with open(src, "wb") as f:
            for b in blobs:
                f.write(struct.pack("<I", len(b)) + b)
        subprocess.run([CFGTOOL, "batch", src, dst], check=True)
        with open(dst) as f:
            lines = f.read().splitlines()
    return ["error" if x.startswith("error") else x for x in lines], lines


def cfgtool_blob(which):
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, which + ".bin")
        subprocess.run([CFGTOOL, which, out], check=True)
        with open(out, "rb") as f:
            return f.read()


# --- a JSON Schema validator for the keywords the generator uses ---------------------------------

ANNOTATIONS = {"$schema", "$id", "$defs", "title", "description", "default", "examples"}


class Validator:
    def __init__(self, root):
        self.root = root

    def ref(self, r):
        assert r.startswith("#/"), r
        s = self.root
        for part in r[2:].split("/"):
            s = s[part]
        return s

    @staticmethod
    def same(a, b):
        return type(a) is type(b) and a == b if isinstance(a, (bool, int, float, str)) or a is None else a == b

    @staticmethod
    def is_type(v, t):
        return {"object": isinstance(v, dict), "array": isinstance(v, list), "string": isinstance(v, str),
                "integer": isinstance(v, int) and not isinstance(v, bool),
                "number": isinstance(v, (int, float)) and not isinstance(v, bool),
                "boolean": isinstance(v, bool), "null": v is None}[t]

    def errors(self, v, s, path="$"):
        """The first fault of v against schema s, or None."""
        if s is True:
            return None
        if s is False:
            return "%s: not allowed" % path
        for k in s:
            assert k in ANNOTATIONS or k in self.KEYWORDS, "unknown keyword %r" % k
        if "$ref" in s:
            e = self.errors(v, self.ref(s["$ref"]), path)
            if e:
                return e
        if "type" in s:
            ts = s["type"] if isinstance(s["type"], list) else [s["type"]]
            if not any(self.is_type(v, t) for t in ts):
                return "%s: %s is not %s" % (path, json.dumps(v), "/".join(ts))
        if "const" in s and not self.same(v, s["const"]):
            return "%s: %s is not %s" % (path, json.dumps(v), json.dumps(s["const"]))
        if "enum" in s and not any(self.same(v, x) for x in s["enum"]):
            return "%s: %s is not one of %s" % (path, json.dumps(v), s["enum"])
        if self.is_type(v, "number"):
            if "minimum" in s and v < s["minimum"]:
                return "%s: %s < %s" % (path, v, s["minimum"])
            if "maximum" in s and v > s["maximum"]:
                return "%s: %s > %s" % (path, v, s["maximum"])
        if isinstance(v, str):
            if "maxLength" in s and len(v) > s["maxLength"]:
                return "%s: longer than %d" % (path, s["maxLength"])
            if "minLength" in s and len(v) < s["minLength"]:
                return "%s: shorter than %d" % (path, s["minLength"])
            if "pattern" in s and not re.search(s["pattern"], v):
                return "%s: does not match %s" % (path, s["pattern"])
        if isinstance(v, dict):
            for k in s.get("required", []):
                if k not in v:
                    return "%s: %s missing" % (path, k)
            for k, needs in s.get("dependentRequired", {}).items():
                if k in v and any(x not in v for x in needs):
                    return "%s: %s needs %s" % (path, k, ", ".join(needs))
            props = s.get("properties", {})
            for k, x in v.items():
                if k in props:
                    e = self.errors(x, props[k], "%s.%s" % (path, k))
                elif "additionalProperties" in s:
                    e = self.errors(x, s["additionalProperties"], "%s.%s" % (path, k))
                else:
                    e = None
                if e:
                    return e
        if isinstance(v, list):
            if "maxItems" in s and len(v) > s["maxItems"]:
                return "%s: more than %d items" % (path, s["maxItems"])
            if "items" in s:
                for i, x in enumerate(v):
                    e = self.errors(x, s["items"], "%s[%d]" % (path, i))
                    if e:
                        return e
        for x in s.get("allOf", []):
            e = self.errors(v, x, path)
            if e:
                return e
        if "anyOf" in s and all(self.errors(v, x, path) for x in s["anyOf"]):
            return "%s: none of anyOf" % path
        if "oneOf" in s and sum(not self.errors(v, x, path) for x in s["oneOf"]) != 1:
            return "%s: not exactly one of oneOf" % path
        if "not" in s and not self.errors(v, s["not"], path):
            return "%s: matches what is not allowed" % path
        if "if" in s:
            branch = s.get("then") if not self.errors(v, s["if"], path) else s.get("else")
            if branch is not None:
                e = self.errors(v, branch, path)
                if e:
                    return e
        return None

    KEYWORDS = {"$ref", "type", "const", "enum", "minimum", "maximum", "maxLength", "minLength", "pattern",
                "required", "dependentRequired", "properties", "additionalProperties", "maxItems", "items", "allOf", "anyOf",
                "oneOf", "not", "if", "then", "else"}

    def check(self, v):
        return self.errors(v, self.root)


# --- tests ----------------------------------------------------------------------------------------

class SchemaTest(unittest.TestCase):
    def test_device_counts_are_the_firmware_s(self):
        with open(CORE_H) as f:
            text = f.read()
        defines = dict(re.findall(r"^#define (CORE_\w+) (\d+)", text, re.M))
        self.assertEqual(int(defines["CORE_PRESETS"]), S.device["presets"])
        self.assertEqual(int(defines["CORE_PAGES"]), S.device["pages"])
        self.assertEqual(int(defines["CORE_STRIPS"]), S.device["strips"])
        self.assertEqual(int(defines["CORE_ROW_BUTTONS"]), len(S.row))


class FileTest(unittest.TestCase):
    def test_examples_are_canonical(self):
        for name, path in EXAMPLES.items():
            with open(path, encoding="utf-8") as f:
                text = f.read()
            m = codec.load(path)
            self.assertEqual(codec.dumps(codec.to_json(m)), text, name)

    def test_the_shortest_file(self):
        m = codec.from_json(doc())
        self.assertEqual(codec.to_json(m), doc())
        blob = codec.encode(m)
        self.assertEqual(len(blob), codec.HEADER)
        self.assertEqual(codec.encode(codec.decode(blob)), blob)

    def test_defaults(self):
        m = codec.from_json(strip_doc(fader={"type": "cc7", "number": 7}, encoder={"type": "cc14", "number": 3},
                                      m={"type": "note", "number": 60}))
        s = first_strip(m)
        fader, enc, btn = s["fader"], s["encoder"], s["m"]
        self.assertEqual(fader["normal"]["mode"], S.kinds["fader"].mode_enum.index("global"))
        self.assertEqual(fader["normal"]["hi"], 127)
        self.assertEqual(fader["shift_kind"], codec.INHERIT)
        self.assertEqual(enc["normal"]["mode"], S.kinds["encoder"].mode_enum.index("acc2"))
        self.assertEqual(enc["normal"]["hi"], 16383)
        self.assertEqual(enc["normal"]["start"], 8192)  # the middle of lo..hi
        self.assertEqual(enc["shift_kind"], codec.OVERRIDE)  # the Normal layer in mode fine (FR-CTL-04)
        self.assertEqual(enc["shift"], dict(enc["normal"], mode=S.kinds["encoder"].mode_enum.index("fine")))
        self.assertEqual(btn["normal"]["mode"], S.kinds["button"].mode_enum.index("momentary"))
        for c in (fader, enc, btn):
            self.assertEqual(c["normal"]["channel"], codec.FIELD["channel"].named[0]["value"])  # "preset"
        m = codec.from_json(strip_doc(encoder={"type": "cc7", "lo": 10, "hi": 20}))
        self.assertEqual(first_strip(m)["encoder"]["normal"]["start"], 15)
        self.assertEqual(first_strip(m)["encoder"]["shift"]["start"], 15)
        # n applies in div and step only, and there it is needed
        m = codec.from_json(strip_doc(encoder={"type": "cc7", "mode": "div", "n": 3}))
        enc = first_strip(m)["encoder"]
        self.assertEqual(enc["normal"]["param"], 3)
        self.assertEqual(enc["shift"]["param"], 0)  # fine takes none
        self.assertError(strip_doc(encoder={"type": "cc7", "mode": "step"}),
                         "presets[0].pages[0].strips[0].encoder.n: needed in mode step")

    def test_preset_channel(self):
        preset = codec.FIELD["channel"].named[0]["value"]
        m = codec.from_json(doc([{}, {"channel": 5, "pages": [{"strips": [
            {"fader": {"type": "cc7"}, "encoder": {"type": "cc7", "channel": 6},
             "m": {"type": "note", "channel": "preset"}}]}]}]))
        self.assertEqual(m["presets"][0]["channel"], 0)  # channel 1 when not written
        self.assertEqual(m["presets"][1]["channel"], 4)
        s = first_strip(m, 1)
        self.assertEqual(s["fader"]["normal"]["channel"], preset)
        self.assertEqual(s["encoder"]["normal"]["channel"], 5)  # fixed: 6 whatever the preset's
        self.assertEqual(s["encoder"]["shift"]["channel"], 5)
        self.assertEqual(s["m"]["normal"]["channel"], preset)
        # "preset" is the default: written only for a fixed channel
        j = codec.to_json(m)
        self.assertEqual(j["presets"][1]["channel"], 5)
        self.assertNotIn("channel", j["presets"][0])
        strip = j["presets"][1]["pages"][0]["strips"][0]
        self.assertNotIn("channel", strip["fader"])
        self.assertNotIn("channel", strip["m"])
        self.assertEqual(strip["encoder"]["channel"], 6)
        self.assertEqual(codec.encode(codec.from_json(j)), codec.encode(m))
        # a Shift layer of its own keeps the preset's channel or a fixed one
        m = codec.from_json(strip_doc(fader={"type": "cc7", "channel": 2, "shift": {"type": "cc7", "number": 9}}))
        f = first_strip(m)["fader"]
        self.assertEqual((f["normal"]["channel"], f["shift"]["channel"]), (1, preset))
        for bad, where in ((doc([{"channel": 17}]), "presets[0].channel"),
                           (doc([{"channel": "preset"}]), "presets[0].channel"),
                           (strip_doc(fader={"type": "cc7", "channel": "foo"}), "strips[0].fader.channel"),
                           (strip_doc(fader={"type": "cc7", "channel": 16.5}), "strips[0].fader.channel")):
            with self.subTest(where=where):
                self.assertError(bad, where)

    def test_shift_layer_name(self):
        # a Shift layer of its own may have a name of its own (names live only in the file, D-8)
        doc_ = strip_doc(fader={"name": "Volume", "type": "cc7", "number": 7, "shift": {"name": "Pan", "type": "cc7", "number": 10}})
        m = codec.from_json(doc_)
        f = first_strip(m)["fader"]
        self.assertEqual((f["name"], f["shift_name"]), ("Volume", "Pan"))
        out = codec.to_json(m)
        self.assertEqual(out["presets"][0]["pages"][0]["strips"][0]["fader"]["shift"],
                         {"name": "Pan", "type": "cc7", "number": 10})
        # not on the device: the binary form is the same with or without it
        plain = strip_doc(fader={"name": "Volume", "type": "cc7", "number": 7, "shift": {"type": "cc7", "number": 10}})
        self.assertEqual(codec.encode(codec.from_json(doc_)), codec.encode(codec.from_json(plain)))
        self.assertEqual(codec.edits(codec.from_json(plain), codec.from_json(doc_)), [])
        # only a layer of its own has one; a name alone is not a layer
        self.assertError(strip_doc(fader={"type": "cc7", "shift": {"name": "Pan"}}), "a layer of its own is written in full")
        self.assertError(strip_doc(fader={"type": "cc7", "shift": {"name": 5, "type": "cc7"}}), "fader.shift.name")
        self.assertError(strip_doc(fader={"type": "cc7", "shift": {"name": "x" * 65, "type": "cc7"}}), "fader.shift.name")
        # an encoder's fine Shift layer with a name stays written: the name is not lost
        fine = strip_doc(encoder={"type": "cc7", "number": 1, "shift": {"name": "Fine cut", "type": "cc7", "number": 1, "mode": "fine"}})
        e = codec.to_json(codec.from_json(fine))["presets"][0]["pages"][0]["strips"][0]["encoder"]
        self.assertEqual(e["shift"], {"name": "Fine cut", "type": "cc7", "number": 1, "mode": "fine"})
        self.assertEqual(codec.to_json(codec.from_json(json.loads(json.dumps(codec.to_json(codec.from_json(fine)))))), codec.to_json(codec.from_json(fine)))

    def test_shift_layer(self):
        # a layer of its own is written in full: nothing comes from the Normal layer
        m = codec.from_json(strip_doc(fader={"type": "cc7", "number": 7, "lo": 10, "shift": {"type": "cc7", "number": 8}}))
        f = first_strip(m)["fader"]
        self.assertEqual(f["shift_kind"], codec.OVERRIDE)
        self.assertEqual((f["shift"]["msg"], f["shift"]["number"], f["shift"]["lo"]), (S.msg.index("cc7"), 8, 0))
        m = codec.from_json(strip_doc(fader={"type": "cc7", "number": 7, "shift": {"type": "pitchbend"}}))
        f = first_strip(m)["fader"]
        self.assertEqual((f["shift"]["msg"], f["shift"]["number"], f["shift"]["hi"]),
                         (S.msg.index("pitchbend"), 0, 16383))
        self.assertError(strip_doc(fader={"type": "cc7", "shift": {"type": "pitchbend", "number": 3}}),
                         "presets[0].pages[0].strips[0].fader.shift.number")
        self.assertError(strip_doc(fader={"type": "cc7", "shift": {"number": 8}}),
                         "presets[0].pages[0].strips[0].fader.shift: a layer of its own is written in full")
        self.assertError(strip_doc(fader={"type": "cc7", "shift": {"type": "none"}}), "use \"block\"")
        self.assertError(doc([{"row": {"rew": {"type": "note", "shift": "block"}}}]), "presets[0].row.rew.shift")
        # an encoder's Shift layer: any type, channel and mode
        m = codec.from_json(strip_doc(encoder={"type": "cc7", "number": 1, "shift": {
            "type": "pitchbend", "channel": 3, "mode": "step", "n": 64}}))
        e = first_strip(m)["encoder"]["shift"]
        self.assertEqual((e["msg"], e["channel"], e["mode"], e["param"], e["start"]),
                         (S.msg.index("pitchbend"), 2, S.kinds["encoder"].mode_enum.index("step"), 64, 8192))
        for k in ("inherit", "block"):
            m = codec.from_json(strip_doc(encoder={"type": "cc7", "shift": k}))
            self.assertEqual(first_strip(m)["encoder"]["shift_kind"], codec.SHIFT.index(k))
            self.assertEqual(codec.to_json(m)["presets"][0]["pages"][0]["strips"][0]["encoder"]["shift"], k)
        # the encoder's default (fine) is left out of a file; anything else is written in full
        for shift in ({"type": "cc7", "number": 1, "mode": "fine"}, {"type": "cc7", "number": 1, "mode": "acc0"}):
            d = strip_doc(encoder={"type": "cc7", "number": 1, "shift": shift})
            j = codec.to_json(codec.from_json(d))
            enc = j["presets"][0]["pages"][0]["strips"][0]["encoder"]
            if shift["mode"] == "fine":
                self.assertNotIn("shift", enc)
            else:
                self.assertEqual(enc["shift"], shift)
        # a fader's Shift layer equal to its Normal one stays written: it is not inherit for the file
        d = strip_doc(fader={"type": "cc7", "number": 7, "shift": {"type": "cc7", "number": 7}})
        self.assertEqual(codec.to_json(codec.from_json(d)), d)

    def test_nrpn(self):
        # a 14-bit parameter number and 14-bit values; written as one number, MSB x 128 + LSB
        d = strip_doc(fader={"type": "nrpn", "number": 0x1234, "lo": 100, "hi": 16000},
                      encoder={"type": "rpn", "number": 2}, m={"type": "nrpn", "number": 16383, "hi": 8192})
        m = codec.from_json(d)
        self.assertEqual(codec.to_json(m), d)
        self.assertEqual(codec.to_json(codec.decode(codec.encode(m))), d)
        f = m["presets"][0]["pages"][0]["strips"][0]["fader"]["normal"]
        self.assertEqual((f["number"], f["lo"], f["hi"]), (0x1234, 100, 16000))

    def test_errors(self):
        cases = [
            (doc(presets=[{"pages": [{"strips": [{"encoder": {"type": "cc14", "number": 40}}]}]}]),
             "strips[0].encoder.number"),
            (strip_doc(encoder={"type": "note"}), "strips[0].encoder.type"),
            (strip_doc(fader={"type": "cc7", "number": 128}), "strips[0].fader.number"),
            (strip_doc(fader={"type": "cc7", "channel": 17}), "strips[0].fader.channel"),
            (strip_doc(fader={"type": "cc7", "channel": 0}), "strips[0].fader.channel"),
            (strip_doc(encoder={"type": "cc7", "n": 3}), "strips[0].encoder.n"),
            (strip_doc(encoder={"type": "cc7", "mode": "div", "n": 0}), "strips[0].encoder.n"),
            (strip_doc(encoder={"type": "cc7", "mode": "div"}), "strips[0].encoder.n: needed"),
            (strip_doc(encoder={"type": "cc7", "mode": "smooth"}), "strips[0].encoder.mode"),
            (strip_doc(fader={"type": "cc7", "start": 3}), "strips[0].fader.start"),
            (strip_doc(fader={"type": "cc7", "default": 3}), "unknown key 'default'"),
            (doc([{"channel": 0}]), "presets[0].channel"),
            (strip_doc(fader={"number": 3}), "no type"),
            (strip_doc(fader={"type": "cc7", "kind": 3}), "unknown key 'kind'"),
            (strip_doc(m={"type": "note", "led": "blink"}), "strips[0].m.led"),
            (strip_doc(m={"type": "note", "noRelease": 1}), "strips[0].m.noRelease"),
            (strip_doc(fader={"type": "pitchbend", "hi": 16384}), "strips[0].fader.hi"),
            (strip_doc(fader={"type": "nrpn", "number": 16384}), "strips[0].fader.number"),
            (strip_doc(m={"type": "rpn", "number": 1}), "strips[0].m.type"),
            (strip_doc(fader={"type": "none", "number": 1}), "type none takes no other field"),
            (doc(global_={}), "unknown key 'global_'"),
            (doc(**{"global": {"pickup": "global"}}), "global.pickup"),
            (doc(**{"global": {"gestureMs": 0}}), "global.gestureMs"),
            (doc([{"shiftMode": "sticky"}]), "presets[0].shiftMode"),
            (doc([{}] * 5), "5 presets"),
            (doc([{"pages": [{}] * 5}]), "5 pages"),
            (doc(name="x" * 65), "name"),
            (doc(id="ABC"), "id"),
            (doc(revision=-1), "revision"),
            ({"schemaVersion": 2}, "schemaVersion"),
            ({}, "schemaVersion"),
        ]
        for obj, where in cases:
            with self.subTest(where=where):
                self.assertError(obj, where)

    def test_newer_versions_say_update(self):
        # Q-29: a file or binary form from a newer release names what to update
        with self.assertRaises(codec.ConfigError) as cm:
            codec.from_json({"schemaVersion": codec.S.version + 1})
        self.assertRegex(str(cm.exception), r"newer version.*update the tool")
        blob = bytearray(codec.encode(codec.from_json({"schemaVersion": codec.S.version})))
        blob[4] = codec.S.binary_version + 1
        with self.assertRaises(codec.ConfigError) as cm:
            codec.decode(bytes(blob))
        self.assertRegex(str(cm.exception), r"binary form .*newer version.*update the tool")
        with self.assertRaises(codec.ConfigError) as cm:
            codec.from_json({"schemaVersion": "1"})
        self.assertIn("this tool reads version", str(cm.exception))

    def test_led_light(self):
        # ledLevel and ledUnlit (SPEC §5.7): ten steps each, full and dark by default, in their own
        # optional global record (tag 0x81) that a firmware before them skips
        m = codec.from_json(doc())
        self.assertEqual((m["global"]["led_level"], m["global"]["led_unlit"]), (10, 0))
        self.assertEqual(len(codec.encode(m)), codec.HEADER)  # the defaults write nothing
        m = codec.from_json(doc(**{"global": {"ledLevel": 4, "ledUnlit": 2}}))
        blob = codec.encode(m)
        self.assertEqual(blob[codec.HEADER:], bytes([0x81, 3, 0x03, 4, 2]))
        self.assertEqual(codec.to_json(codec.decode(blob))["global"], {"ledLevel": 4, "ledUnlit": 2})
        m = codec.from_json(doc(**{"global": {"save": "auto", "ledUnlit": 1}}))
        self.assertEqual(codec.encode(m)[codec.HEADER:], bytes([1, 2, 0x40, 1, 0x81, 2, 0x02, 1]))
        self.assertError(doc(**{"global": {"ledLevel": 0}}), "global.ledLevel")
        self.assertError(doc(**{"global": {"ledLevel": 11}}), "global.ledLevel")
        self.assertError(doc(**{"global": {"ledUnlit": 11}}), "global.ledUnlit")
        head = codec.encode(codec.from_json(doc()))
        def body(recs):
            rest = head[16:36] + recs
            return head[:8] + struct.pack("<II", len(recs), zlib.crc32(rest)) + rest
        for bad in (bytes([0x81, 2, 0x01, 0]), bytes([0x81, 2, 0x02, 11]), bytes([0x81, 1, 0x04]),
                    bytes([0x81, 0]), bytes([0x81, 2, 0x01, 5, 0x81, 2, 0x01, 5])):
            with self.assertRaises(codec.ConfigError, msg=bad.hex()):
                codec.decode(body(bad))
        # an edit of the LED light alone: its record only, with preset 0
        f = codec.load(EXAMPLES["factory"])
        g = copy.deepcopy(f)
        g["global"]["led_level"] = 3
        self.assertEqual(codec.edits(f, g), [(0, bytes([0x81, 2, 0x01, 3]))])
        g["global"]["led_level"] = 10
        self.assertEqual(codec.edits(f, g), [])

    def assertError(self, obj, where):
        with self.assertRaises(codec.ConfigError) as cm:
            codec.from_json(json.loads(json.dumps(obj)))
        self.assertIn(where, str(cm.exception))

    def test_random_round_trips(self):
        rnd = random.Random(SEED)
        for i in range(12):
            m = random_model(rnd)
            blob = codec.encode(m)
            j = codec.to_json(m)
            again = codec.from_json(json.loads(codec.dumps(j)))
            self.assertEqual(codec.encode(again), blob, "seed %d model %d" % (SEED, i))
            self.assertEqual(codec.to_json(again), j)
            self.assertEqual(codec.encode(codec.decode(blob)), blob)

    def test_binary_errors(self):
        blob = codec.encode(codec.load(EXAMPLES["test"]))
        bad = [blob[:35], b"SMCX" + blob[4:], blob[:-1], blob + b"\0", blob[:20] + bytes([blob[20] ^ 1]) + blob[21:]]
        for b in bad:
            with self.assertRaises(codec.ConfigError):
                codec.decode(b)


class JsonSchemaTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(JSON_SCHEMA) as f:
            cls.v = Validator(json.load(f))

    def test_examples_are_valid(self):
        for name, path in EXAMPLES.items():
            with open(path) as f:
                self.assertIsNone(self.v.check(json.load(f)), name)

    def test_random_configurations_are_valid(self):
        rnd = random.Random(SEED + 1)
        for i in range(8):
            j = json.loads(codec.dumps(codec.to_json(random_model(rnd))))
            self.assertIsNone(self.v.check(j), "seed %d model %d" % (SEED + 1, i))

    def test_faults_are_refused(self):
        cases = [
            strip_doc(encoder={"type": "cc14", "number": 40}),
            strip_doc(encoder={"type": "note"}),
            strip_doc(fader={"type": "cc7", "number": 128}),
            strip_doc(fader={"type": "cc7", "channel": 17}),
            strip_doc(encoder={"type": "cc7", "n": 3}),
            strip_doc(fader={"number": 3}),
            strip_doc(fader={"type": "cc7", "kind": 3}),
            strip_doc(fader={"type": "pitchbend", "hi": 16384}),
            strip_doc(fader={"type": "nrpn", "number": 16384}),
            strip_doc(fader={"type": "nrpn", "hi": 16384}),
            strip_doc(m={"type": "rpn", "number": 1}),
            strip_doc(fader={"type": "pitchbend", "number": 1}),
            strip_doc(m={"type": "note", "led": "blink"}),
            strip_doc(fader={"type": "cc7", "shift": "sometimes"}),
            doc([{"row": {"rew": {"type": "note", "shift": "block"}}}]),
            doc(**{"global": {"pickup": "global"}}),
            strip_doc(fader={"type": "none", "channel": 2}),
            strip_doc(fader={"type": "cc7", "shift": {"type": "none"}}),
            strip_doc(encoder={"type": "cc7", "shift": {"mode": "acc2", "n": 3}}),
            strip_doc(encoder={"type": "cc7", "shift": {"number": 3}}),
            strip_doc(encoder={"type": "cc7", "mode": "div"}),
            strip_doc(encoder={"type": "cc7", "shift": {"type": "cc7", "mode": "step"}}),
            strip_doc(fader={"type": "cc7", "channel": "foo"}),
            strip_doc(fader={"type": "cc7", "start": 3}),
            doc([{"channel": 17}]),
            doc([{"channel": "preset"}]),
            {"schemaVersion": 2},
            doc(id="ABC"),
        ]
        for obj in cases:
            with self.subTest(obj=obj):
                self.assertIsNotNone(self.v.check(obj))

    def test_never_stricter_than_the_codec(self):
        rnd = random.Random(SEED + 2)
        with open(EXAMPLES["test"]) as f:
            base = json.load(f)
        pool = [0, 1, 2, 5, 16, 17, 31, 32, 127, 128, 16383, 16384, -1, "cc7", "cc14", "pitchbend", "note",
                "none", "relative2c", "acc0", "div", "step", "toggle", "jump", "global", True, False, "on",
                "value", None, "inherit", "block", {"number": 5}, {"type": "none"}, {"mode": "div", "n": 3},
                "preset", "fine", "acc2", {"type": "cc7", "mode": "div", "n": 3}, {"type": "pitchbend"},
                {"type": "cc14", "number": 5, "mode": "fine", "channel": "preset"}]
        accepted = refused = 0
        for _ in range(400):
            d = json.loads(json.dumps(base))
            for _ in range(rnd.randint(1, 2)):
                p = grow(d.setdefault("presets", []), rnd.randrange(S.device["presets"]))
                if rnd.random() < 0.2:
                    obj = p.setdefault("row", {}).setdefault(rnd.choice(codec.ROW_KEYS), {})
                else:
                    page = grow(p.setdefault("pages", []), rnd.randrange(S.device["pages"]))
                    strip = grow(page.setdefault("strips", []), rnd.randrange(S.device["strips"]))
                    obj = strip.setdefault(rnd.choice(codec.STRIP_KEYS), {})
                k = rnd.choice(codec.JSON_KEYS + ["shift"])
                if rnd.random() < 0.2:
                    obj.pop(k, None)
                else:
                    obj[k] = rnd.choice(pool)
            try:
                codec.from_json(d)
            except codec.ConfigError:
                refused += 1
                continue
            accepted += 1
            self.assertIsNone(self.v.check(d), json.dumps(d)[:200])
        self.assertGreater(accepted, 40)
        self.assertGreater(refused, 40)


@unittest.skipUnless(os.access(CFGTOOL, os.X_OK), "build/host/cfgtool not built (make -C firmware test)")
class AgreementTest(unittest.TestCase):
    def test_builtin_configurations_are_the_examples(self):
        for name, path in EXAMPLES.items():
            blob = cfgtool_blob(name)
            self.assertEqual(codec.encode(codec.load(path)), blob, name)

    def test_damaged_forms(self):
        rnd = random.Random(SEED + 3)
        bases = [cfgtool_blob("factory"), cfgtool_blob("test")]
        bases += [codec.encode(random_model(rnd)) for _ in range(4)]
        blobs = [damage(rnd.choice(bases), rnd) for _ in range(500)]
        c, raw = c_results(blobs)
        self.assertEqual(len(c), len(blobs))
        ok = 0
        for i, b in enumerate(blobs):
            py = py_result(b)
            self.assertEqual(py, c[i], "seed %d blob %d: C %s" % (SEED + 3, i, raw[i][:60]))
            ok += py != "error"
        self.assertGreater(ok, 50)
        self.assertGreater(len(blobs) - ok, 200)

    def test_random_configurations(self):
        rnd = random.Random(SEED + 4)
        blobs = [codec.encode(random_model(rnd)) for _ in range(16)]
        c, _ = c_results(blobs)
        for i, b in enumerate(blobs):
            self.assertEqual(c[i], "ok " + b.hex(), "seed %d model %d" % (SEED + 4, i))


def c_patch(base, edits):
    """cfgtool patch: the canonical form after the edits, by the device's codec."""
    with tempfile.TemporaryDirectory() as d:
        b, e = os.path.join(d, "base.bin"), os.path.join(d, "edits.bin")
        with open(b, "wb") as f:
            f.write(base)
        with open(e, "wb") as f:
            for p, recs in edits:
                f.write(struct.pack("<BH", p, len(recs)) + recs)
        r = subprocess.run([CFGTOOL, "patch", b, e], capture_output=True, text=True)
    return r.stdout.strip()


class EditTest(unittest.TestCase):
    """Live edits (CFG_SET, P-1): codec.edits() of two configurations, applied by the device's
    cfgbin_patch() to the first, give the second."""

    def check(self, old, new, what):
        edits = codec.edits(old, new)
        for p, recs in edits:
            self.assertLessEqual(len(recs), codec.EDIT_MAX)
        want = dict(new, id=old["id"], revision=old["revision"])
        self.assertEqual(c_patch(codec.encode(old), edits), "ok " + codec.encode(want).hex(), what)
        return edits

    def test_pairs(self):
        rnd = random.Random(SEED + 5)
        f, t = codec.load(EXAMPLES["factory"]), codec.load(EXAMPLES["test"])
        self.check(f, t, "factory -> test")
        self.check(t, f, "test -> factory")
        self.assertEqual(codec.edits(f, f), [])
        for i in range(12):
            a, b = random_model(rnd), random_model(rnd)
            self.check(a, b, "seed %d pair %d" % (SEED + 5, i))

    def test_small(self):
        f = codec.load(EXAMPLES["factory"])
        g = copy.deepcopy(f)
        g["presets"][0]["pages"][1]["strips"][0]["encoder"]["normal"]["number"] = 77
        g["presets"][0]["pages"][1]["strips"][0]["encoder"]["name"] = "Cutoff"
        e = self.check(f, g, "one control")
        self.assertEqual(len(e), 1)
        self.assertEqual(e[0][0], 0)
        self.assertEqual(e[0][1][:4], bytes([3, e[0][1][1], 1, 8]))  # a Normal layer: page 2, encoder 1
        h = copy.deepcopy(g)
        h["presets"][2]["channel"] = 9
        h["global"]["save"] = 1
        h["presets"][3]["row"][0]["normal"] = codec.empty_assign()
        e = self.check(g, h, "settings and a control to none")
        self.assertEqual([p for p, _ in e], [2, 3])
        self.assertEqual(e[0][1][:4], bytes([1, 2, 0x40, 1]))  # the global record first
        self.assertIn(bytes([3, 4, 255, 0, 0, 0]), e[1][1])   # play: none
        n = copy.deepcopy(f)
        n["name"] = "renamed"
        self.assertEqual(codec.edits(f, n), [])               # names are not on the device


class PlatformImportTest(unittest.TestCase):
    """Offline CLIs and WinMM routing with POSIX modules unavailable; no native API calls."""

    def run_without_posix(self, script, args, blocked=()):
        code = ("import runpy, sys; "
                "sys.modules.update({name: None for name in sys.argv[1].split(',')}); "
                "sys.argv = sys.argv[2:]; runpy.run_path(sys.argv[0], run_name='__main__')")
        result = subprocess.run([sys.executable, "-I", "-c", code,
                                 ",".join(("termios", "tty") + blocked), script] + args,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_offline_configuration_commands_without_transports(self):
        with tempfile.TemporaryDirectory() as d:
            formatted, binary, decoded = [os.path.join(d, name) for name in ("a.json", "a.bin", "b.json")]
            script = os.path.join(HERE, "smcconf.py")
            for args in (["validate", EXAMPLES["factory"]],
                         ["format", EXAMPLES["factory"], "-o", formatted],
                         ["encode", formatted, "-o", binary],
                         ["decode", binary, "-o", decoded]):
                self.run_without_posix(script, args, ("devlink", "hostmidi", "hostserial"))
            self.assertEqual(codec.encode(codec.load(decoded)), codec.encode(codec.load(formatted)))

    def test_log_decode_without_posix(self):
        with tempfile.TemporaryDirectory() as d:
            dictionary, stream = os.path.join(d, "dict.json"), os.path.join(d, "stream.bin")
            with open(dictionary, "w") as f:
                json.dump({"format": 1, "sites": {}}, f)
            with open(stream, "wb"):
                pass
            self.run_without_posix(os.path.join(ROOT, "tools", "smclog", "smclog.py"),
                                   ["decode", "--dict", dictionary, stream])

    def test_winmm_selection_without_posix(self):
        code = """
import sys
sys.modules.update({'termios': None, 'tty': None, 'hostserial': None})
sys.path.insert(0, sys.argv[1])
import devlink
from unittest.mock import patch
calls = []
class FakeWinMM(devlink.hostmidi.Port):
    def __init__(self, name):
        super().__init__(name)
        calls.append(name)
with patch.object(devlink.hostmidi.platform, 'system', return_value='Windows'), \\
     patch.dict(devlink.hostmidi.BACKENDS, {'Windows': (FakeWinMM, None)}):
    with devlink.Device(midi='Fake MIDI') as device:
        assert device.midi.name == 'Fake MIDI'
assert calls == ['Fake MIDI']
"""
        result = subprocess.run([sys.executable, "-I", "-c", code, HERE],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class DirectImportTest(unittest.TestCase):
    """Imported helpers work without main() initializing transport globals; no hardware."""

    def test_open_device_without_main(self):
        import argparse
        import devlink
        import smcconf
        from unittest.mock import patch

        args = argparse.Namespace(port="unused", timeout=0.25, midi=None, where="unused")
        with patch.object(devlink, "Device") as device:
            self.assertIs(smcconf.open_device(args), device.return_value)
        device.assert_called_once_with("unused", timeout=0.25, midi=None)

    def test_device_commands_report_protocol_errors_without_main(self):
        import argparse
        import contextlib
        import devlink
        import io
        import smcconf
        from unittest.mock import patch

        args = argparse.Namespace(file=EXAMPLES["factory"], where="unused", preset=1, page=None)
        for name in ("info", "pull", "push", "edit", "save", "revert", "state", "select", "watch"):
            with self.subTest(command=name):
                errors = io.StringIO()
                with patch.object(smcconf, "open_device") as open_device, contextlib.redirect_stderr(errors):
                    open_device.return_value.__enter__.return_value.info.side_effect = devlink.DeviceError("no reply")
                    self.assertEqual(getattr(smcconf, "cmd_" + name)(args), 1)
                self.assertEqual(errors.getvalue(), "smcconf: unused: no reply\n")

    def test_control_text_without_transports(self):
        import smcconf
        from unittest.mock import patch

        with patch.dict(sys.modules, {"devlink": None, "hostmidi": None, "hostserial": None}):
            self.assertEqual(smcconf.control_text(None), "none")
            self.assertEqual(smcconf.control_text({"preset": 0, "page": 255, "control": 1, "layer": 0}),
                             "preset 1, the row's control 1, Normal")
            self.assertEqual(smcconf.control_text({"preset": 2, "page": 1, "control": 8, "layer": 1}),
                             "preset 3, page 2's control 8, Shift")


class CliTest(unittest.TestCase):
    def run_cli(self, *args):
        r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "smcconf.py")] + list(args),
                           capture_output=True, text=True)
        return r.returncode, r.stdout + r.stderr

    def test_commands(self):
        with tempfile.TemporaryDirectory() as d:
            j, b, j2 = (os.path.join(d, x) for x in ("a.json", "a.bin", "b.json"))
            with open(j, "w") as f:
                json.dump(strip_doc(fader={"number": 7, "type": "cc7"}), f)  # not canonical
            self.assertEqual(self.run_cli("validate", j)[0], 0)
            code, out = self.run_cli("format", j, "--check")
            self.assertEqual(code, 1, out)
            self.assertEqual(self.run_cli("format", j, "--in-place")[0], 0)
            self.assertEqual(self.run_cli("format", j, "--check")[0], 0)
            self.assertEqual(self.run_cli("encode", j, "-o", b)[0], 0)
            self.assertEqual(self.run_cli("decode", b, "-o", j2)[0], 0)
            with open(j) as f1, open(j2) as f2:
                self.assertEqual(f1.read(), f2.read())
            self.assertEqual(self.run_cli("diff", j, b), (0, ""))
            code, out = self.run_cli("diff", j, EXAMPLES["factory"])
            self.assertEqual(code, 1)
            self.assertIn("presets[0].pages[0].strips[0].fader.normal.number: 7 -> 20", out)
            code, out = self.run_cli("size", EXAMPLES["factory"])
            self.assertEqual(code, 0)
            self.assertIn("preset 4:", out)
            with open(j, "w") as f:
                json.dump(strip_doc(fader={"type": "cc7", "number": 200}), f)
            code, out = self.run_cli("validate", j, EXAMPLES["test"])
            self.assertEqual(code, 1)
            self.assertIn("presets[0].pages[0].strips[0].fader.number", out)
            self.assertIn("test.json: ok", out)
            self.assertEqual(self.run_cli("validate", os.path.join(d, "missing.json"))[0], 2)


if __name__ == "__main__":
    unittest.main(verbosity=1)

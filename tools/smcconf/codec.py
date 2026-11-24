"""Configuration files and the device's binary form (docs/config.md; SPEC D-4, FR-APP-21).

A configuration is held as a model with the device's values (the C structures of
firmware/core/core_config.h: channel 0-15, enums as numbers) plus the names, which live only in
files (D-8):

    {"name", "id" (16 bytes), "revision", "global": {c field: value},
     "presets": [{"name", "channel", "shift_mode", "note_off",
                  "pages": [{"name", "strips": [{"fader": ctl, "encoder": ctl, "m": ctl, "s": ctl,
                                                 "r": ctl, "select": ctl}]}],
                  "row": [ctl] * 11}]}
    ctl = {"name", "normal": assign, "shift": assign, "shift_kind", "shift_name"}; assign = {c field: value}

An assignment's channel is 0-15, or 16 (CORE_CHANNEL_PRESET, "preset" in files): the preset's.

from_json() and decode() check everything and raise ConfigError with the path of the fault;
to_json() and encode() give the canonical form (defaults left out, fixed order), which the C codec
(firmware/config/cfgbin.c) writes byte for byte the same. Standard library only.
"""
import json
import re
import struct
import zlib

import schema as schema_mod


class ConfigError(Exception):
    def __init__(self, path, msg):
        super().__init__("%s: %s" % (path or "(top)", msg))
        self.path, self.msg = path, msg


S = schema_mod.load()
FIELDS = [f.c for f in S.assign_fields]
FIELD = {f.c: f for f in S.assign_fields}
FLAGS = FIELD["flags"]
MEMBERS = {m["json"]: m for m in FLAGS.members}
JSON_KEYS = []  # an assignment's keys in a file, in order
for _f in S.assign_fields:
    JSON_KEYS += [m["json"] for m in _f.members] if _f.members else [_f.json]
KIND_OF = {c["json"]: S.kinds[c["kind"]] for c in S.strip_controls}
STRIP_KEYS = [c["json"] for c in S.strip_controls]
ROW_KEYS = [b["json"] for b in S.row]
NONE = S.msg.index("none")
SHIFT = S.enums["shiftKind"]
INHERIT, OVERRIDE, BLOCK = SHIFT.index("inherit"), SHIFT.index("override"), SHIFT.index("block")
BUTTON = S.kinds["button"]
TAG = S.tags
ROW_PAGE = S.binary["rowPage"]
OPTIONAL = S.binary["optionalTagBit"]
HEADER = S.binary["headerSize"]
MAGIC = S.binary["magic"].encode()
UUID_RE = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")


def device(dev=None):
    d = dict(S.device)
    d.update(dev or {})
    return d


# --- the model ------------------------------------------------------------------------------

def empty_assign():
    return dict.fromkeys(FIELDS, 0)


def empty_control():
    return {"name": None, "normal": empty_assign(), "shift": empty_assign(), "shift_kind": INHERIT, "shift_name": None}


def empty_config(dev=None):
    dev = device(dev)
    g = {f.c: S.default(f) for f in S.global_fields}
    presets = []
    for _ in range(dev["presets"]):
        p = {"name": None, "row": [empty_control() for _ in ROW_KEYS],
             "pages": [{"name": None, "strips": [{k: empty_control() for k in STRIP_KEYS}
                                                 for _ in range(dev["strips"])]}
                       for _ in range(dev["pages"])]}
        for f in S.preset_fields:
            p[f.c] = S.default(f)
        presets.append(p)
    return {"name": None, "id": bytes(16), "revision": 0, "global": g, "presets": presets}


def controls(preset):
    """(page, control id, kind, nav, ctl) of a preset, in the binary order; page ROW_PAGE: the row."""
    for pg, page in enumerate(preset["pages"]):
        for c in S.strip_controls:
            for n, strip in enumerate(page["strips"]):
                yield pg, c["id"] + n, S.kinds[c["kind"]], False, strip[c["json"]]
    for i, ctl in enumerate(preset["row"]):
        yield ROW_PAGE, i, BUTTON, S.row_nav(i), ctl


def control_at(preset, page, cid, dev):
    """(kind, nav, ctl) at a binary address, or None."""
    if page == ROW_PAGE:
        if cid < len(ROW_KEYS):
            return BUTTON, S.row_nav(cid), preset["row"][cid]
        return None
    group, n = divmod(cid, 8)
    if page >= dev["pages"] or group >= len(S.strip_controls) or n >= dev["strips"]:
        return None
    c = S.strip_controls[group]
    return S.kinds[c["kind"]], False, preset["pages"][page]["strips"][n][c["json"]]


def fine_of(normal):
    """The encoder's default Shift layer: the Normal layer in mode fine (FR-CTL-04)."""
    a = dict(normal)
    a["mode"] = S.kinds["encoder"].mode_enum.index(S.fine["mode"])
    a["param"] = 0
    return a


def default_shift(kind, normal):
    if kind.shift_default == "fine" and normal["msg"] != NONE:
        return OVERRIDE, fine_of(normal)
    return INHERIT, empty_assign()


# --- checks on the device's values (as cfgbin_check_struct) ------------------------------------

def where(kind):
    return ("an " if kind.name[0] in "aeiou" else "a ") + kind.name


def check_assign(kind, a, path):
    msg = a["msg"]
    if msg not in kind.types:
        raise ConfigError(path, "type %s is not for %s (%s)" % (
            S.msg.json[msg] if msg < len(S.msg) else msg, where(kind), ", ".join(S.msg.json[t] for t in kind.types)))
    if msg == NONE:
        bad = [f for f in FIELDS if a[f]]
        if bad:
            raise ConfigError(path, "type none with %s set" % ", ".join(bad))
        return
    mode = a["mode"]
    if mode >= len(kind.mode_enum):
        raise ConfigError(path, "mode %d is not one of %s's" % (mode, kind.name))
    for f in S.assign_fields:
        v = a[f.c]
        if f.c in ("msg", "mode"):
            continue
        if not S.applies(f, kind, msg, mode):
            if v:
                raise ConfigError(path, "%s does not apply here (it is %d)" % (f.json or f.c, v))
            continue
        if f.members:
            known = 0
            for m in f.members:
                known |= m["mask"]
                if v & m["mask"] not in m["values"].values():
                    raise ConfigError(path, "%s: bits %#x" % (m["json"], v & m["mask"]))
            if v & ~known:
                raise ConfigError(path, "unknown flags %#x" % (v & ~known))
            continue
        lo, hi = f.min - f.offset, S.field_max(f, msg) - f.offset
        if not lo <= v <= hi and v not in [n["value"] for n in f.named]:
            raise ConfigError(path, "%s %d is outside %d-%d" % (f.json, v + f.offset, lo + f.offset, hi + f.offset))


def check_control(kind, nav, ctl, path):
    check_assign(kind, ctl["normal"], path)
    k = ctl["shift_kind"]
    if k not in (INHERIT, OVERRIDE, BLOCK):
        raise ConfigError(path, "shift kind %d" % k)
    if nav and k != INHERIT:
        raise ConfigError(path, "the Shift layer of a navigation button is the firmware's (FR-SH-02)")
    if k != OVERRIDE:
        if any(ctl["shift"].values()):
            raise ConfigError(path + ".shift", "set although the Shift layer is %s" % SHIFT.json[k])
        return
    if ctl["shift"]["msg"] == NONE:
        raise ConfigError(path + ".shift", "an override that sends nothing: use \"block\"")
    check_assign(kind, ctl["shift"], path + ".shift")


def check_setting(f, v, path, enum=None):
    if enum is not None:
        allowed = [i for i in range(len(enum)) if enum.json[i] not in f.exclude]
        if v not in allowed:
            raise ConfigError(path, "%s %s is not one of %s" % (f.json, v, ", ".join(enum.json[i] for i in allowed)))
    elif not f.min - f.offset <= v <= f.max - f.offset:
        raise ConfigError(path, "%s %d is outside %d-%d" % (f.json, v + f.offset, f.min, f.max))


def check_model(m):
    for f in S.global_fields:
        check_setting(f, m["global"][f.c], "global." + f.json, S.enums.get(f.enum))
    for p, preset in enumerate(m["presets"]):
        for f in S.preset_fields:
            check_setting(f, preset[f.c], "presets[%d].%s" % (p, f.json), S.enums.get(f.enum))
        for pg, cid, kind, nav, ctl in controls(preset):
            check_control(kind, nav, ctl, path_of(p, pg, cid))


def path_of(p, page, cid):
    if page == ROW_PAGE:
        return "presets[%d].row.%s" % (p, ROW_KEYS[cid])
    group, n = divmod(cid, 8)
    return "presets[%d].pages[%d].strips[%d].%s" % (p, page, n, STRIP_KEYS[group])


# --- files ------------------------------------------------------------------------------------

def is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def version_text(found, reads, who="tool"):
    """A version this tool does not read, said so that a musician knows what to update (Q-29; the
    same words as the editor's versionText in app/editor/js/schema.js)."""
    if isinstance(found, int) and not isinstance(found, bool) and found > reads:
        return "%d: made by a newer version of better-smc-mixer; this %s reads version %d: update the %s" % (
            found, who, reads, who)
    return "%s: this %s reads version %d" % (json.dumps(found), who, reads)


def need(cond, path, msg):
    if not cond:
        raise ConfigError(path, msg)


def keys_ok(obj, allowed, path):
    need(isinstance(obj, dict), path, "an object is needed")
    extra = [k for k in obj if k not in allowed]
    need(not extra, path, "unknown key%s %s (known: %s)" % (
        "s" if len(extra) > 1 else "", ", ".join(map(repr, extra)), ", ".join(allowed)))


def name_of(obj, path):
    v = obj.get("name")
    if v is not None:
        need(isinstance(v, str) and len(v) <= S.name_max, path + ".name", "a string of at most %d characters" % S.name_max)
    return v


def json_value(f, v, path, enum=None, lo=None, hi=None):
    """A file value to the device's."""
    if enum is not None:
        allowed = [n for n in enum.json if n not in f.exclude]
        need(v in allowed, path, "%s is not one of %s" % (json.dumps(v), ", ".join(allowed)))
        return enum.index(v)
    lo = f.min if lo is None else lo
    hi = f.max if hi is None else hi
    words = "".join(" or %s" % n["json"] for n in f.named)
    for n in f.named:
        if v == n["json"]:
            return n["value"]
    need(is_int(v), path, "a whole number%s is needed, not %s" % (words, json.dumps(v)))
    need(lo <= v <= hi, path, "%d is outside %d-%d%s" % (v, lo, hi, words))
    return v - f.offset


def file_value(f, v, enum=None):
    """A device value to the file's."""
    if enum is not None:
        return enum.json[v]
    for n in f.named:
        if v == n["value"]:
            return n["json"]
    return v + f.offset


def resolve(kind, written, path):
    """A layer's written fields (from the file) to the device's values: defaults for what is not
    written; a written field that does not apply, or a required one missing, is an error."""
    need("type" in written or not written, path, "no type")
    t = written.get("type", "none")
    msg = json_value(FIELD["msg"], t, path + ".type", S.msg)
    need(msg in kind.types, path + ".type", "%s is not for %s (%s)" % (
        t, where(kind), ", ".join(S.msg.json[i] for i in kind.types)))
    if msg == NONE:
        extra = [k for k in written if k != "type"]
        need(not extra, path, "type none takes no other field (%s)" % ", ".join(extra))
        return empty_assign()
    mode = kind.mode_default
    if "mode" in written:
        mode = json_value(FIELD["mode"], written["mode"], path + ".mode", kind.mode_enum)
    a = {"msg": msg, "mode": mode}
    for f in S.assign_fields:
        if f.c in ("msg", "mode"):
            continue
        keys = [m["json"] for m in f.members] if f.members else [f.json]
        applies = S.applies(f, kind, msg, mode)
        for k in keys:
            if k in written and not applies:
                raise ConfigError(path + "." + k, "does not apply to %s %s in mode %s" % (
                    S.msg.json[msg], kind.name, kind.mode_enum.json[mode]))
        if not applies:
            a[f.c] = 0
            continue
        if f.members:
            v = 0
            for m in f.members:
                if m["json"] in written:
                    w = written[m["json"]]
                    key = json.dumps(w) if isinstance(w, bool) else w
                    need(isinstance(key, str) and key in m["values"], path + "." + m["json"],
                         "%s is not one of %s" % (json.dumps(w), ", ".join(m["values"])))
                    v |= m["values"][key]
            a[f.c] = v
        elif f.json in written:
            a[f.c] = json_value(f, written[f.json], path + "." + f.json, hi=S.field_max(f, msg))
        else:
            need(not f.required, path + "." + f.json, "needed in mode %s" % kind.mode_enum.json[mode])
            a[f.c] = S.default(f, kind, msg, mode, a)
    return a


def control_from_json(obj, kind, nav, path):
    ctl = empty_control()
    if obj is None:
        return ctl
    keys_ok(obj, ["name", "shift"] + JSON_KEYS, path)
    ctl["name"] = name_of(obj, path)
    ctl["normal"] = resolve(kind, {k: v for k, v in obj.items() if k in JSON_KEYS}, path)
    if "shift" not in obj:
        ctl["shift_kind"], ctl["shift"] = default_shift(kind, ctl["normal"])
        return ctl
    need(not nav, path + ".shift", "the Shift layer of a navigation button is the firmware's (FR-SH-02)")
    s = obj["shift"]
    if s in ("inherit", "block"):
        ctl["shift_kind"] = SHIFT.index(s)
        return ctl
    need(isinstance(s, dict), path + ".shift", "\"inherit\", \"block\" or a layer of its own")
    keys_ok(s, ["name"] + JSON_KEYS, path + ".shift")
    need("type" in s, path + ".shift", "a layer of its own is written in full: its type at least")
    # its own name (only a layer of its own has one; names live only in the file, D-8)
    ctl["shift_name"] = name_of(s, path + ".shift")
    ctl["shift"] = resolve(kind, {k: v for k, v in s.items() if k in JSON_KEYS}, path + ".shift")
    need(ctl["shift"]["msg"] != NONE, path + ".shift", "a layer that sends nothing: use \"block\"")
    ctl["shift_kind"] = OVERRIDE
    return ctl


def from_json(obj, dev=None):
    """A configuration file's object (json.load) to the model; ConfigError on any fault."""
    dev = device(dev)
    m = empty_config(dev)
    keys_ok(obj, ["schemaVersion", "id", "revision", "name", "global", "presets"], "")
    need("schemaVersion" in obj, "schemaVersion", "missing")
    need(obj["schemaVersion"] == S.version, "schemaVersion", version_text(obj["schemaVersion"], S.version))
    m["name"] = name_of(obj, "")
    if "id" in obj:
        v = obj["id"]
        need(isinstance(v, str) and UUID_RE.match(v), "id", "a UUID in lower case, 8-4-4-4-12 hex digits")
        m["id"] = bytes.fromhex(v.replace("-", ""))
    if "revision" in obj:
        v = obj["revision"]
        need(is_int(v) and 0 <= v <= 0xFFFFFFFF, "revision", "a whole number 0-4294967295")
        m["revision"] = v
    g = obj.get("global", {})
    keys_ok(g, [f.json for f in S.global_fields], "global")
    for f in S.global_fields:
        if f.json in g:
            m["global"][f.c] = json_value(f, g[f.json], "global." + f.json, S.enums.get(f.enum))
    presets = obj.get("presets", [])
    need(isinstance(presets, list), "presets", "a list is needed")
    need(len(presets) <= dev["presets"], "presets", "%d presets; the device has %d" % (len(presets), dev["presets"]))
    for p, po in enumerate(presets):
        path = "presets[%d]" % p
        preset = m["presets"][p]
        if po is None:
            continue
        keys_ok(po, ["name"] + [f.json for f in S.preset_fields] + ["row", "pages"], path)
        preset["name"] = name_of(po, path)
        for f in S.preset_fields:
            if f.json in po:
                preset[f.c] = json_value(f, po[f.json], path + "." + f.json, S.enums.get(f.enum))
        row = po.get("row", {})
        keys_ok(row, ROW_KEYS, path + ".row")
        for i, k in enumerate(ROW_KEYS):
            preset["row"][i] = control_from_json(row.get(k), BUTTON, S.row_nav(i), path + ".row." + k)
        pages = po.get("pages", [])
        need(isinstance(pages, list), path + ".pages", "a list is needed")
        need(len(pages) <= dev["pages"], path + ".pages", "%d pages; the device has %d" % (len(pages), dev["pages"]))
        for pg, pgo in enumerate(pages):
            ppath = "%s.pages[%d]" % (path, pg)
            if pgo is None:
                continue
            keys_ok(pgo, ["name", "strips"], ppath)
            page = preset["pages"][pg]
            page["name"] = name_of(pgo, ppath)
            strips = pgo.get("strips", [])
            need(isinstance(strips, list) and len(strips) <= dev["strips"], ppath + ".strips",
                 "a list of at most %d strips" % dev["strips"])
            for n, so in enumerate(strips):
                spath = "%s.strips[%d]" % (ppath, n)
                if so is None:
                    continue
                keys_ok(so, STRIP_KEYS, spath)
                for k in STRIP_KEYS:
                    page["strips"][n][k] = control_from_json(so.get(k), KIND_OF[k], False, spath + "." + k)
    check_model(m)
    return m


def assign_to_json(kind, a):
    """The written form of a layer: only what differs from the defaults."""
    out = {}
    msg, mode = a["msg"], a["mode"]
    if msg == NONE:
        return out
    for f in S.assign_fields:
        if not S.applies(f, kind, msg, mode) and f.c != "msg":
            continue
        if f.members:
            for m in f.members:
                v = a["flags"] & m["mask"]
                if v != 0:
                    out[m["json"]] = next(json.loads(k) if k in ("true", "false") else k
                                          for k, x in m["values"].items() if x == v)
            continue
        v = a[f.c]
        if v == S.default(f, kind, msg, mode, a):  # a required field's default, 0, is never valid
            continue
        out[f.json] = file_value(f, v, S.field_enum(f, kind))
    return out


def control_to_json(kind, nav, ctl):
    out = {}
    if ctl["name"] is not None:
        out["name"] = ctl["name"]
    out.update(assign_to_json(kind, ctl["normal"]))
    named = ctl["shift_kind"] == OVERRIDE and ctl.get("shift_name") is not None
    if named or (ctl["shift_kind"], ctl["shift"]) != default_shift(kind, ctl["normal"]):
        if ctl["shift_kind"] == OVERRIDE:
            out["shift"] = {"name": ctl["shift_name"]} if ctl.get("shift_name") is not None else {}
            out["shift"].update(assign_to_json(kind, ctl["shift"]))
        else:
            out["shift"] = SHIFT.json[ctl["shift_kind"]]
    return out


def trim(items):
    while items and not items[-1]:
        items.pop()
    return items


def to_json(m):
    """The canonical file object of a model."""
    out = {"schemaVersion": S.version}
    if any(m["id"]):
        h = m["id"].hex()
        out["id"] = "%s-%s-%s-%s-%s" % (h[:8], h[8:12], h[12:16], h[16:20], h[20:])
    if m["revision"]:
        out["revision"] = m["revision"]
    if m["name"] is not None:
        out["name"] = m["name"]
    g = {}
    for f in S.global_fields:
        v = m["global"][f.c]
        if v != S.default(f):
            e = S.enums.get(f.enum)
            g[f.json] = file_value(f, v, e)
    if g:
        out["global"] = g
    presets = []
    for p, preset in enumerate(m["presets"]):
        po = {}
        if preset["name"] is not None:
            po["name"] = preset["name"]
        for f in S.preset_fields:
            v = preset[f.c]
            if v != S.default(f):
                po[f.json] = file_value(f, v, S.enums.get(f.enum))
        row = {}
        for i, k in enumerate(ROW_KEYS):
            c = control_to_json(BUTTON, S.row_nav(i), preset["row"][i])
            if c:
                row[k] = c
        if row:
            po["row"] = row
        pages = []
        for pg, page in enumerate(preset["pages"]):
            pgo = {}
            if page["name"] is not None:
                pgo["name"] = page["name"]
            strips = []
            for n, strip in enumerate(page["strips"]):
                so = {}
                for k in STRIP_KEYS:
                    c = control_to_json(KIND_OF[k], False, strip[k])
                    if c:
                        so[k] = c
                strips.append(so)
            if trim(strips):
                pgo["strips"] = strips
            pages.append(pgo)
        if trim(pages):
            po["pages"] = pages
        presets.append(po)
    if trim(presets):
        out["presets"] = presets
    return out


def dumps(obj):
    """Stable formatting: an object or list of plain values on one line, others indented; so each
    control is one line in a diff."""
    def flat(v):
        return not isinstance(v, (dict, list)) or all(not isinstance(x, (dict, list)) for x in (
            v.values() if isinstance(v, dict) else v))

    def fmt(v, ind):
        if flat(v):
            return json.dumps(v, ensure_ascii=False, separators=(", ", ": "))
        pad = "  " * (ind + 1)
        if isinstance(v, dict):
            items = ["%s%s: %s" % (pad, json.dumps(k, ensure_ascii=False), fmt(x, ind + 1)) for k, x in v.items()]
            return "{\n" + ",\n".join(items) + "\n" + "  " * ind + "}"
        items = [pad + fmt(x, ind + 1) for x in v]
        return "[\n" + ",\n".join(items) + "\n" + "  " * ind + "]"

    return fmt(obj, 0) + "\n"


# --- the binary form ----------------------------------------------------------------------------

def pack_fields(fields, values, mask):
    out = bytearray()
    for i, f in enumerate(fields):
        if mask >> i & 1:
            out += struct.pack("<B" if f.size == 1 else "<H", values[i])
    return out


def assign_values(a):
    return [a[f] for f in FIELDS]


def record(tag, payload):
    assert len(payload) < 256
    return bytes([tag, len(payload)]) + payload


def global_record(m):
    """The global record (its mask 0 when every setting is the default)."""
    gv = [m["global"][f.c] for f in S.global_fields]
    gmask = sum(1 << i for i, f in enumerate(S.global_fields) if gv[i] != S.default(f))
    return record(TAG["global"], bytes([gmask]) + pack_fields(S.global_fields, gv, gmask)), gmask


def preset_record(p, preset):
    """Preset p's record (its mask 0 when every setting is the default)."""
    pv = [preset[f.c] for f in S.preset_fields]
    pmask = sum(1 << i for i, f in enumerate(S.preset_fields) if pv[i] != S.default(f))
    return record(TAG["preset"], bytes([p, pmask]) + pack_fields(S.preset_fields, pv, pmask)), pmask


def control_records(pg, cid, kind, ctl):
    """A control's records in the canonical form: its Normal layer unless none, its Shift layer
    unless the default; empty when both are."""
    out = bytearray()
    a = ctl["normal"]
    if a["msg"] != NONE:
        vals = assign_values(a)
        mask = sum(1 << i for i, f in enumerate(S.assign_fields)
                   if vals[i] != S.default(f, kind, a["msg"], a["mode"], a))
        out += record(TAG["assign"], bytes([pg, cid]) + struct.pack("<H", mask) +
                      pack_fields(S.assign_fields, vals, mask))
    if (ctl["shift_kind"], ctl["shift"]) != default_shift(kind, a):
        payload = bytes([pg, cid, ctl["shift_kind"]])
        if ctl["shift_kind"] == OVERRIDE:
            vals, base = assign_values(ctl["shift"]), assign_values(a)
            mask = sum(1 << i for i in range(len(vals)) if vals[i] != base[i])
            payload += struct.pack("<H", mask) + pack_fields(S.assign_fields, vals, mask)
        out += record(TAG["shift"], payload)
    return bytes(out)


def encode(m):
    """The canonical binary form of a model (checked first)."""
    check_model(m)
    body = bytearray()
    g, gmask = global_record(m)
    if gmask:
        body += g
    for p, preset in enumerate(m["presets"]):
        recs = b"".join(control_records(pg, cid, kind, ctl) for pg, cid, kind, nav, ctl in controls(preset))
        prec, pmask = preset_record(p, preset)
        if pmask or recs:
            body += prec + recs
    head = MAGIC + struct.pack("<BBHII", S.binary_version, HEADER, 0, len(body), 0)
    rest = struct.pack("<I", m["revision"]) + m["id"] + bytes(body)
    return head[:12] + struct.pack("<I", zlib.crc32(rest)) + rest


# --- live edits (CFG_SET, P-1) ----------------------------------------------------------------

EDIT_MAX = 184  # a CFG_SET's records: the link's 192-byte command less kind, seq, cmd, revision, preset


def same_control(a, b):
    return (a["normal"], a["shift_kind"], a["shift"]) == (b["normal"], b["shift_kind"], b["shift"])


def edits(old, new, limit=EDIT_MAX):
    """The live edits that turn model old into new (docs/config.md, "Edits"): a list of (preset,
    records), each at most `limit` bytes, for CFG_SET in this order. Every setting record or
    control that differs is written whole, as the canonical form writes it (a control that has
    none: a Normal layer of no fields, none; settings at their defaults: a record of mask 0);
    names do not count. The global record goes first, with the first preset that changes (else
    preset 0). [] when they play the same."""
    check_model(new)
    per = []
    for p, (po, pn) in enumerate(zip(old["presets"], new["presets"])):
        recs = []
        prec, _ = preset_record(p, pn)
        if prec != preset_record(p, po)[0]:
            recs.append(prec)
        for (pg, cid, kind, nav, cn), (_, _, _, _, co) in zip(controls(pn), controls(po)):
            if not same_control(cn, co):
                recs.append(control_records(pg, cid, kind, cn) or
                            record(TAG["assign"], bytes([pg, cid, 0, 0])))
        if recs:
            per.append((p, recs))
    g, _ = global_record(new)
    if g != global_record(old)[0]:
        if per:
            per[0][1].insert(0, g)
        else:
            per.append((0, [g]))
    out = []
    for p, recs in per:
        for r in recs:
            assert len(r) <= limit
            if out and out[-1][0] == p and len(out[-1][1]) + len(r) <= limit:
                out[-1] = (p, out[-1][1] + r)
            else:
                out.append((p, r))
    return out


def unpack_fields(fields, mask, data, off, path):
    vals = {}
    for i, f in enumerate(fields):
        if mask >> i & 1:
            need(off + f.size <= len(data), path, "the record ends inside its fields")
            vals[i] = data[off] if f.size == 1 else struct.unpack_from("<H", data, off)[0]
            off += f.size
    need(off == len(data), path, "%d bytes after the fields" % (len(data) - off))
    return vals


def decode(blob, dev=None):
    """The binary form to the model (names empty); ConfigError with the offset on any fault."""
    dev = device(dev)
    blob = bytes(blob)
    need(len(blob) >= HEADER, "header", "%d bytes: shorter than the header" % len(blob))
    need(blob[:4] == MAGIC, "header", "not a configuration (magic %r)" % blob[:4])
    version, hsize, _, body_len, crc = struct.unpack_from("<BBHII", blob, 4)
    need(version == S.binary_version, "header", "binary form " + version_text(version, S.binary_version))
    need(hsize >= HEADER, "header", "header size %d" % hsize)
    need(hsize + body_len == len(blob), "header", "length %d + %d, but %d bytes" % (hsize, body_len, len(blob)))
    need(zlib.crc32(blob[16:]) == crc, "header", "CRC mismatch")
    m = empty_config(dev)
    m["revision"] = struct.unpack_from("<I", blob, 16)[0]
    m["id"] = blob[20:36]
    off = hsize
    preset = None
    seen_global = False
    seen_presets = set()
    seen = set()  # (preset, page, control, layer)
    last = None
    while off < len(blob):
        here = "offset %d" % off
        need(off + 2 <= len(blob), here, "a record header past the end")
        tag, ln = blob[off], blob[off + 1]
        data = blob[off + 2:off + 2 + ln]
        need(len(data) == ln, here, "a record of %d bytes past the end" % ln)
        off += 2 + ln
        this, last = last, None
        if tag == TAG["global"]:
            need(not seen_global, here, "a second global record")
            seen_global = True
            need(ln >= 1, here, "an empty global record")
            mask = data[0]
            need(mask >> len(S.global_fields) == 0, here, "unknown global fields %#x" % mask)
            vals = unpack_fields(S.global_fields, mask, data, 1, here)
            for i, v in vals.items():
                f = S.global_fields[i]
                m["global"][f.c] = v
            for f in S.global_fields:
                check_setting(f, m["global"][f.c], here + " global." + f.json, S.enums.get(f.enum))
        elif tag == TAG["preset"]:
            need(ln >= 2, here, "a preset record of %d bytes" % ln)
            p, mask = data[0], data[1]
            need(p < dev["presets"], here, "preset %d; the device has %d" % (p + 1, dev["presets"]))
            need(p not in seen_presets, here, "a second record of preset %d" % (p + 1))
            seen_presets.add(p)
            need(mask >> len(S.preset_fields) == 0, here, "unknown preset fields %#x" % mask)
            vals = unpack_fields(S.preset_fields, mask, data, 2, here)
            preset = m["presets"][p]
            pidx = p
            for i, f in enumerate(S.preset_fields):
                v = vals.get(i, S.default(f))
                check_setting(f, v, "%s presets[%d].%s" % (here, p, f.json), S.enums.get(f.enum))
                preset[f.c] = v
        elif tag in (TAG["assign"], TAG["shift"]):
            need(preset is not None, here, "a control before any preset record")
            need(ln >= 2, here, "a control record of %d bytes" % ln)
            page, cid = data[0], data[1]
            at = control_at(preset, page, cid, dev)
            need(at is not None, here, "no control %d on page %d" % (cid, page))
            kind, nav, ctl = at
            path = "%s %s" % (here, path_of(pidx, page, cid))
            key = (pidx, page, cid)
            if tag == TAG["assign"]:
                need(key + (0,) not in seen, path, "a second Normal layer")
                need(key + (1,) not in seen, path, "the Normal layer after the Shift layer")
                seen.add(key + (0,))
                need(ln >= 4, path, "no field mask")
                mask = struct.unpack_from("<H", data, 2)[0]
                need(mask >> len(FIELDS) == 0, path, "unknown fields %#x" % mask)
                vals = unpack_fields(S.assign_fields, mask, data, 4, path)
                msg = vals.get(0, NONE)
                mode = vals.get(3, kind.mode_default if msg != NONE else 0)
                a = empty_assign()
                a.update(msg=msg, mode=mode)
                if msg in kind.types and mode < len(kind.mode_enum):  # else check_assign says why
                    for i, f in enumerate(S.assign_fields):
                        a[f.c] = vals[i] if i in vals else mode if f.c == "mode" else S.default(f, kind, msg, mode, a)
                check_assign(kind, a, path)
                ctl["normal"] = a
                last = key
            else:
                need(key + (1,) not in seen, path, "a second Shift layer")
                need(key + (0,) not in seen or this == key, path, "the Shift layer apart from its Normal layer")
                seen.add(key + (1,))
                need(ln >= 3, path, "no Shift kind")
                k = data[2]
                need(k in (INHERIT, OVERRIDE, BLOCK), path, "Shift kind %d" % k)
                need(not nav, path, "the Shift layer of a navigation button is the firmware's (FR-SH-02)")
                ctl["shift_kind"] = k
                ctl["shift"] = empty_assign()
                if k == OVERRIDE:
                    need(ln >= 5, path, "no field mask")
                    mask = struct.unpack_from("<H", data, 3)[0]
                    need(mask >> len(FIELDS) == 0, path, "unknown fields %#x" % mask)
                    vals = unpack_fields(S.assign_fields, mask, data, 5, path)
                    s = dict(ctl["normal"])
                    for i, v in vals.items():
                        s[FIELDS[i]] = v
                    ctl["shift"] = s
                else:
                    need(ln == 3, path, "%d bytes after an %s" % (ln - 3, SHIFT.json[k]))
        else:
            need(tag & OPTIONAL, here, "unknown record type %d" % tag)
    for p, preset in enumerate(m["presets"]):
        for pg, cid, kind, nav, ctl in controls(preset):
            if (p, pg, cid, 1) not in seen:
                ctl["shift_kind"], ctl["shift"] = default_shift(kind, ctl["normal"])
            check_control(kind, nav, ctl, path_of(p, pg, cid))
    return m


def load(path, dev=None):
    """A file: a binary (by its magic) or JSON."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] == MAGIC:
        return decode(data, dev)
    try:
        obj = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as e:
        raise ConfigError("", "not JSON: %s" % e)
    return from_json(obj, dev)


def size_report(m):
    """Bytes of the binary form, in all and per preset (SPEC Q-3)."""
    blob = encode(m)
    per = []
    for p in range(len(m["presets"])):
        one = empty_config({"presets": len(m["presets"]), "pages": len(m["presets"][p]["pages"]),
                            "strips": len(m["presets"][p]["pages"][0]["strips"])})
        one["presets"][p] = m["presets"][p]
        per.append(len(encode(one)) - HEADER)
    return len(blob), per

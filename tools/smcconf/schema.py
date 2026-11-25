"""The configuration schema (config/schema.json, SPEC D-4) as Python objects, for the codec
(codec.py) and the generator (gen.py). docs/config.md describes the schema and the formats.

The rules every user of the schema shares:

- an assignment field applies to a control kind, a message type and a mode as its "applies" says
  ("notNone": any type but none; "withNumber": types with a number; "withValue": types with a value
  range); a field that does not apply is absent from files and binaries and zero on the device;
- its default: a number, "kindMode" (the kind's default mode), "typeMax" (the type's largest value),
  "middle" (the middle of the layer's lo and hi, rounded up), a named value's or an enum value's
  name; a "required" field has none: where it applies it must be given (0 on the device, which its
  minimum refuses);
- "named" values of a numeric field are words in files for device values outside its numbers (the
  channel "preset": 16).

Standard library only.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
SCHEMA_PATH = os.path.join(ROOT, "config", "schema.json")


class SchemaError(Exception):
    pass


class Enum:
    def __init__(self, d):
        self.name, self.c, self.doc = d["name"], d["c"], d["doc"]
        self.values = d["values"]
        self.by_json = {v["json"]: i for i, v in enumerate(self.values)}
        self.json = [v["json"] for v in self.values]

    def __len__(self):
        return len(self.values)

    def index(self, json_name):
        return self.by_json[json_name]


class Kind:
    def __init__(self, d, enums, msg):
        self.name = d["name"]
        self.mode_enum = enums[d["modeEnum"]]
        self.mode_default = self.mode_enum.index(d["modeDefault"])
        self.shift_default = d["shiftDefault"]  # "inherit" or "fine"
        self.types = [msg.index(t) for t in d["types"]]


class Field:
    """A field of the assignment, the preset or the global settings."""

    def __init__(self, d):
        self.c, self.json, self.size = d["c"], d["json"], d["size"]
        self.doc = d.get("doc", "")
        self.enum = d.get("enum")
        self.min, self.max = d.get("min", 0), d.get("max")
        self.offset = d.get("offset", 0)  # file value = C value + offset
        self.required = d.get("required", False)
        self.default = None if self.required else d.get("default", 0)
        self.named = d.get("named", [])  # [{"json", "value" (device), "c", "doc"}]
        self.applies = d.get("applies", {})
        self.exclude = d.get("exclude", [])
        self.members = d.get("members")
        self.record = d.get("record", "global")  # a global setting's binary record (docs/config.md)


class Schema:
    def __init__(self, path=SCHEMA_PATH):
        with open(path) as f:
            d = json.load(f)
        self.raw = d
        self.version = d["schemaVersion"]
        self.binary_version = d["binaryVersion"]
        self.device = d["device"]
        self.enums = {e["name"]: Enum(e) for e in d["enums"]}
        self.msg = self.enums["msg"]
        self.kinds = {k["name"]: Kind(k, self.enums, self.msg) for k in d["kinds"]}
        self.assign_fields = [Field(f) for f in d["assignment"]["fields"]]
        self.preset_fields = [Field(f) for f in d["preset"]["fields"]]
        self.global_fields = [Field(f) for f in d["global"]["fields"]]  # the file's: every record's
        self.strip_controls = d["strip"]["controls"]
        self.row = d["row"]["buttons"]
        self.fine = d["control"]["shift"]["fine"]
        self.binary = d["binary"]
        self.tags = {r["name"]: r["tag"] for r in d["binary"]["records"]}
        # The global settings' binary records, in the canonical order: (name, fields). The global
        # record's mask is one byte, so later settings go into optional records an older firmware
        # skips ("global2", tag 0x81).
        self.global_record_fields = []
        for f in self.global_fields:
            if not self.global_record_fields or self.global_record_fields[-1][0] != f.record:
                self.global_record_fields.append((f.record, []))
            self.global_record_fields[-1][1].append(f)
        self.name_max = d["file"]["nameMax"]
        self.check()

    def check(self):
        names = [f.c for f in self.assign_fields]
        if names != ["msg", "channel", "number", "mode", "param", "flags", "lo", "hi", "start"]:
            raise SchemaError("the assignment's fields are those of struct core_assign, in its order: %s" % names)
        for k in self.kinds.values():
            if self.msg.index("none") not in k.types:
                raise SchemaError("kind %s: every kind takes the type none" % k.name)
        if self.fine != {"mode": "fine"}:
            raise SchemaError("the encoder's default Shift layer is the Normal layer in mode fine (FR-CTL-04)")
        for f in self.preset_fields + self.global_fields:
            if not f.c:
                raise SchemaError("%s: every setting is the device's" % f.json)
        names = [n for n, _ in self.global_record_fields]
        if len(set(names)) != len(names) or names[0] != "global" or any(n not in self.tags for n in names) \
                or len(names) > 8:  # the firmware keeps one bit a record (cfgbin.c's seen_global)
            raise SchemaError("global settings: each record's fields together, the global record's first")
        for n, fields in self.global_record_fields:
            if len(fields) > 8:
                raise SchemaError("global record %s: more than 8 fields for its mask byte" % n)
            if n != "global" and not self.tags[n] & self.binary["optionalTagBit"]:
                raise SchemaError("global record %s: a later record must be optional" % n)
        if [c["id"] for c in self.strip_controls] != [0, 8, 16, 24, 32, 40]:
            raise SchemaError("strip control ids: 8 per group")

    # --- message types ---------------------------------------------------------------------

    def type_max(self, msg):
        return self.msg.values[msg].get("max", 0)

    def number_max(self, msg):
        return self.msg.values[msg].get("numberMax")

    def type_set(self, name):
        none = self.msg.index("none")
        if name == "notNone":
            return {i for i in range(len(self.msg)) if i != none}
        if name == "withNumber":
            return {i for i, v in enumerate(self.msg.values) if "numberMax" in v}
        if name == "withValue":
            return {i for i, v in enumerate(self.msg.values) if "max" in v}
        raise SchemaError("type set %r" % name)

    # --- assignment fields -----------------------------------------------------------------

    def applies(self, field, kind, msg, mode):
        a = field.applies
        if not a:
            return True
        if "types" in a and msg not in self.type_set(a["types"]):
            return False
        if "kinds" in a and kind.name not in a["kinds"]:
            return False
        if "modes" in a and kind.mode_enum.json[mode] not in a["modes"]:
            return False
        return True

    def device_max(self, field, msg):
        """The largest device value: the numbers' and the named values'."""
        return max([self.field_max(field, msg) - field.offset] + [n["value"] for n in field.named])

    def field_max(self, field, msg):
        if field.max == "numberMax":
            return self.number_max(msg)
        if field.max == "typeMax":
            return self.type_max(msg)
        return field.max

    def field_enum(self, field, kind=None):
        if field.enum == "kindMode":
            return kind.mode_enum
        return self.enums[field.enum] if field.enum else None

    def default(self, field, kind=None, msg=0, mode=0, layer=None):
        """The C value of a field when it is not given (0 for a field that does not apply, or a
        required one). layer: the assignment so far, for "middle" (its lo and hi)."""
        if kind is not None and not self.applies(field, kind, msg, mode):
            return 0
        d = field.default
        if d is None:
            return 0
        if d == "middle":
            return (layer["lo"] + layer["hi"] + 1) // 2
        for n in field.named:
            if d == n["json"]:
                return n["value"]
        if d == "kindMode":
            return kind.mode_default
        if d == "typeMax":
            return self.type_max(msg)
        if field.enum and isinstance(d, str):
            return self.field_enum(field, kind).index(d)
        if field.members:
            return 0
        return d - field.offset if field.offset and isinstance(d, int) else d

    def row_nav(self, index):
        return bool(self.row[index].get("nav"))


_schema = None


def load():
    global _schema
    if _schema is None:
        _schema = Schema()
    return _schema

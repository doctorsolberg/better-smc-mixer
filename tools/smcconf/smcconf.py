"""smcconf: configuration files of the SMC-Mixer firmware (docs/config.md; SPEC FR-APP-21, FR-APP-23).

Usage: python3 -I tools/smcconf/smcconf.py COMMAND ...

  validate FILE...
        Check configurations: JSON files (config/smc-config.schema.json's format) or binary forms
        (what the device stores, by their magic). A line each: "FILE: ok ..." or "FILE: PATH: what
        is wrong". Exit 1 if any is not valid.
  format FILE [-o OUT | --in-place | --check]
        The canonical JSON of a configuration: defaults left out, a fixed key order, a control a
        line. --check: exit 1 if FILE is not already in that form (for git hooks and CI).
  encode FILE -o OUT
        The binary form, byte for byte what the firmware writes (firmware/config/cfgbin.c).
  decode FILE [-o OUT]
        A binary form as canonical JSON (without names: the device does not keep them, D-8).
  diff A B
        What differs between two configurations (JSON or binary, in any form), a line per field
        with its path. Exit 0: the same; 1: different.
  size FILE
        Bytes of the binary form, in all and per preset (SPEC Q-3).
  info (--port PORT | --midi MIDI)
        The device's configuration protocol: versions, limits, the running configuration's source,
        ID and revision, and its flash storage: none, clean (it plays what it starts with), unsaved
        (it waits for save), waiting (for the quiet pause: its setting save is auto), writing,
        failed; the saved configuration's generation.
  pull (--port PORT | --midi MIDI) [-o OUT] [--binary]
        The device's running configuration, as canonical JSON (without names: the device does not
        keep them, D-8) or, with --binary, its binary form.
  push FILE (--port PORT | --midi MIDI) [--no-verify] [--save | --wait-save]
        Replace the device's configuration with FILE (JSON or binary): checked here, sent in chunks,
        checked again on the device and applied between two rounds of its control task (the core
        restarts on it); then read back and compared (unless --no-verify). It is not saved to flash
        unless --save (SAVE, then waits for it), or FILE's setting save is "auto": the device
        saves it 3 s after the last change and the last touch of a control, and --wait-save waits
        for that (up to 30 s; docs/config.md, "Storage").
  edit FILE (--port PORT | --midi MIDI) [--save]
        Make the device play FILE without a restart (P-1): its running configuration read, what
        differs sent as live edits (CFG_SET; docs/config.md, "Edits"), read back and compared (its
        ID stays the device's). --save as for push.
  save (--port PORT | --midi MIDI)
        Save the running configuration to flash now (Q-26), and wait for it.
  revert (--port PORT | --midi MIDI)
        Put back what the device starts with: its saved configuration, or the factory one.
  state (--port PORT | --midi MIDI)
        The panel's state (P-2, P-6): the preset and the page, Shift, Bluetooth, the touches and the
        last one's control, the storage, the running configuration's revision and ID.
  select PRESET [PAGE] (--port PORT | --midi MIDI)
        Switch the mixer to PRESET (1-4) and PAGE (1-4; left out: the preset's last page), as its
        own buttons do (P-3).
  watch (--port PORT | --midi MIDI) [--seconds N]
        The device's events (P-2) as they come, a line each, for N seconds (default 10).

PORT is the device's command link (its USB CDC port, docs/link.md, "Configuration"); MIDI is the same
link over SysEx on a MIDI port (docs/link.md, "Over SysEx"): the mixer's "SMC-Mixer Config" over USB
or its BLE-MIDI port, by the name the OS gives it, or fd:N / raw:PATH (tools/hostmidi). Exit 1: not
valid, refused by the device, no reply or a mismatch; 2: usage or a file that cannot be read.
Standard library only.
"""
import argparse
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import codec  # noqa: E402


def unreadable(path, e):
    print("smcconf: %s: %s" % (path, e.strerror or e), file=sys.stderr)
    sys.exit(2)


def load(path):
    try:
        return codec.load(path)
    except OSError as e:
        unreadable(path, e)


def read_bytes(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError as e:
        unreadable(path, e)


def canonical(m):
    return codec.dumps(codec.to_json(m))


def write(path, data):
    mode = "wb" if isinstance(data, bytes) else "w"
    if path in (None, "-"):
        if isinstance(data, bytes):
            sys.stdout.buffer.write(data)
        else:
            sys.stdout.write(data)
        return
    with open(path, mode, **({} if isinstance(data, bytes) else {"encoding": "utf-8", "newline": "\n"})) as f:
        f.write(data)


def cmd_validate(a):
    bad = 0
    for path in a.files:
        try:
            m = load(path)
        except codec.ConfigError as e:
            print("%s: %s" % (path, e))
            bad += 1
            continue
        size, _ = codec.size_report(m)
        print("%s: ok (%d bytes in binary form, revision %d)" % (path, size, m["revision"]))
    return 1 if bad else 0


def cmd_format(a):
    try:
        text = canonical(load(a.file))
    except codec.ConfigError as e:
        print("%s: %s" % (a.file, e), file=sys.stderr)
        return 1
    if a.check:
        same = read_bytes(a.file) == text.encode("utf-8")
        if not same:
            print("%s: not in the canonical form (smcconf format --in-place)" % a.file)
        return 0 if same else 1
    write(a.file if a.in_place else a.output, text)
    return 0


def cmd_encode(a):
    try:
        blob = codec.encode(load(a.file))
    except codec.ConfigError as e:
        print("%s: %s" % (a.file, e), file=sys.stderr)
        return 1
    write(a.output, blob)
    return 0


def cmd_decode(a):
    try:
        m = codec.decode(read_bytes(a.file))
    except codec.ConfigError as e:
        print("%s: %s" % (a.file, e), file=sys.stderr)
        return 1
    write(a.output, canonical(m))
    return 0


def flatten(v, path, out):
    """Leaves of a canonical JSON object by path; a control's layer is one leaf per key."""
    if isinstance(v, dict):
        for k, x in v.items():
            flatten(x, "%s.%s" % (path, k) if path else k, out)
    elif isinstance(v, list):
        for i, x in enumerate(v):
            flatten(x, "%s[%d]" % (path, i), out)
    else:
        out[path] = v
    return out


def cmd_diff(a):
    try:
        ma, mb = load(a.a), load(a.b)
    except codec.ConfigError as e:
        print("smcconf: %s" % e, file=sys.stderr)
        return 2
    # Compare the device's values (resolved, defaults filled in), then the names, so that two
    # files writing the same thing differently are the same.
    fa, fb = flatten(model_view(ma), "", {}), flatten(model_view(mb), "", {})
    lines = []
    for k in sorted(set(fa) | set(fb), key=path_key):
        if fa.get(k) != fb.get(k):
            lines.append("%s: %s -> %s" % (k, show(fa.get(k)), show(fb.get(k))))
    for line in lines:
        print(line)
    return 1 if lines else 0


def model_view(m):
    """The model with readable names, for diff: every field of every layer, defaults included."""
    S = codec.S
    out = {"name": m["name"], "id": m["id"].hex(), "revision": m["revision"], "global": {}}
    for f in S.global_fields:
        out["global"][f.json] = codec.file_value(f, m["global"][f.c], S.enums.get(f.enum))
    out["presets"] = []
    for preset in m["presets"]:
        po = {"name": preset["name"]}
        for f in S.preset_fields:
            po[f.json] = codec.file_value(f, preset[f.c], S.enums.get(f.enum))
        po["row"] = {k: control_view(codec.BUTTON, preset["row"][i]) for i, k in enumerate(codec.ROW_KEYS)}
        po["pages"] = [{"name": page["name"],
                        "strips": [{k: control_view(codec.KIND_OF[k], strip[k]) for k in codec.STRIP_KEYS}
                                   for strip in page["strips"]]} for page in preset["pages"]]
        out["presets"].append(po)
    return out


def layer_view(kind, a):
    S = codec.S
    if a["msg"] == codec.NONE:
        return {"type": "none"}
    v = {}
    for f in S.assign_fields:
        if not S.applies(f, kind, a["msg"], a["mode"]):
            continue
        x = a[f.c]
        if f.members:
            for mem in f.members:
                name = next(k for k, val in mem["values"].items() if val == x & mem["mask"])
                v[mem["json"]] = json.loads(name) if name in ("true", "false") else name
        else:
            v[f.json] = codec.file_value(f, x, S.field_enum(f, kind))
    return v


def control_view(kind, ctl):
    v = {"name": ctl["name"], "normal": layer_view(kind, ctl["normal"])}
    k = codec.SHIFT.json[ctl["shift_kind"]]
    v["shift"] = layer_view(kind, ctl["shift"]) if ctl["shift_kind"] == codec.OVERRIDE else k
    return v


def path_key(p):
    """Paths in document order: numbers in brackets compare as numbers."""
    return [int(x) if x.isdigit() else x for x in re.split(r"\[(\d+)\]", p)]


def show(v):
    return "(none)" if v is None else json.dumps(v, ensure_ascii=False)


def cmd_size(a):
    try:
        m = load(a.file)
    except codec.ConfigError as e:
        print("%s: %s" % (a.file, e), file=sys.stderr)
        return 1
    total, per = codec.size_report(m)
    print("%d bytes in binary form (header %d)" % (total, codec.HEADER))
    for i, n in enumerate(per):
        print("  preset %d: %d bytes" % (i + 1, n))
    return 0


def open_device(a):
    import devlink

    try:
        dev = devlink.Device(a.port, timeout=a.timeout, midi=a.midi)
    except OSError as e:
        unreadable(a.where, e)
    except devlink.hostmidi.MidiError as e:
        print("smcconf: %s" % e, file=sys.stderr)
        sys.exit(2)
    return dev


def device_info(dev):
    info = dev.info()
    dev.check_compatible(info, codec.S.binary_version, codec.S.version)
    return info


def info_text(i):
    return ("configuration protocol %d, binary form %d, schema %d; at most %d bytes, chunks of %d; %s; "
            "running: %s, revision %d, ID %s; storage: %s, saved generation %d" % (
                i["version"], i["binary_version"], i["schema_version"], i["max_length"], i["chunk"], i["state"],
                i["source"], i["revision"], i["id"].hex() if i["id"] else "none", i["store"], i["generation"]))


def cmd_info(a):
    import devlink

    with open_device(a) as dev:
        try:
            i = dev.info()
        except devlink.DeviceError as e:
            print("smcconf: %s: %s" % (a.where, e), file=sys.stderr)
            return 1
    print(info_text(i))
    return 0


def cmd_pull(a):
    import devlink

    with open_device(a) as dev:
        try:
            info = device_info(dev)
            blob = dev.pull(info["chunk"])
        except devlink.DeviceError as e:
            print("smcconf: %s: %s" % (a.where, e), file=sys.stderr)
            return 1
    try:
        m = codec.decode(blob)
    except codec.ConfigError as e:  # the device's encoder and ours disagree
        print("smcconf: %s: the configuration read is not valid here: %s" % (a.where, e), file=sys.stderr)
        return 1
    write(a.output, blob if a.binary else canonical(m))
    print("pulled %d bytes: %s, revision %d" % (len(blob), info["source"], m["revision"]), file=sys.stderr)
    return 0


def cmd_push(a):
    import devlink

    try:
        m = load(a.file)
        blob = codec.encode(m)
    except codec.ConfigError as e:
        print("%s: %s" % (a.file, e), file=sys.stderr)
        return 1
    with open_device(a) as dev:
        try:
            info = device_info(dev)
            if len(blob) > info["max_length"]:
                raise devlink.DeviceError("%d bytes; the device takes at most %d" % (len(blob), info["max_length"]))
            after = dev.push(blob, chunk=min(info["chunk"], 128))
            if after["source"] != "received" or (after["id"] or bytes(16)) != m["id"]:
                raise devlink.DeviceError("the device runs another configuration after the push: %s"
                                          % info_text(after))
            if not a.no_verify:
                back = dev.pull(info["chunk"])
                want = codec.encode(dict(m, revision=after["revision"]))
                if back != want and codec.encode(codec.decode(back)) != want:
                    raise devlink.DeviceError("the configuration read back differs from the one sent")
                dev.abort()  # the snapshot is read: the device may save
            saved = dev.save() if a.save else dev.wait_saved(info["generation"]) if a.wait_save else None
        except (devlink.DeviceError, codec.ConfigError) as e:
            print("smcconf: %s: %s" % (a.where, e), file=sys.stderr)
            return 1
    print("pushed %d bytes, revision %d: applied%s%s" % (
        len(blob), after["revision"], "" if a.no_verify else ", read back the same",
        ", saved (generation %d)" % saved["generation"] if saved else ""))
    return 0


def cmd_edit(a):
    import devlink

    try:
        m = load(a.file)
        codec.encode(m)
    except codec.ConfigError as e:
        print("%s: %s" % (a.file, e), file=sys.stderr)
        return 1
    with open_device(a) as dev:
        try:
            info = device_info(dev)
            old = codec.decode(dev.pull(info["chunk"]))
            dev.abort()
            edits = codec.edits(old, m)
            if edits:
                dev.edit(edits, m["revision"])
            back = codec.decode(dev.pull(info["chunk"]))
            dev.abort()
            # The snapshot is authoritative. Older firmware can report idle before SET has taken
            # effect, so use the read (GET_BEGIN waits for the controller) to confirm content.
            revision = back["revision"]
            want = codec.encode(dict(m, id=old["id"], revision=revision))
            if codec.encode(back) != want or (not edits and revision != old["revision"]):
                raise devlink.DeviceError("the configuration read back differs from FILE")
            saved = dev.save() if a.save else None
        except (devlink.DeviceError, codec.ConfigError) as e:
            print("smcconf: %s: %s" % (a.where, e), file=sys.stderr)
            return 1
    print("edited: %d command%s, %d bytes, revision %d, without a restart; read back the same%s" % (
        len(edits), "" if len(edits) == 1 else "s", sum(len(r) for _, r in edits), revision,
        ", saved (generation %d)" % saved["generation"] if saved else ""))
    return 0


def cmd_save(a):
    import devlink

    with open_device(a) as dev:
        try:
            device_info(dev)
            i = dev.save()
        except devlink.DeviceError as e:
            print("smcconf: %s: %s" % (a.where, e), file=sys.stderr)
            return 1
    print("saved: generation %d, revision %d" % (i["generation"], i["revision"]))
    return 0


def cmd_revert(a):
    import devlink

    with open_device(a) as dev:
        try:
            device_info(dev)
            i = dev.revert()
        except devlink.DeviceError as e:
            print("smcconf: %s: %s" % (a.where, e), file=sys.stderr)
            return 1
    print("reverted: %s, revision %d" % (i["source"], i["revision"]))
    return 0


def control_text(t):
    """A control as the panel's state and the sent entries name it (preset, page, control, layer)."""
    return "none" if t is None else "preset %d, %s control %d, %s" % (
        t["preset"] + 1, "the row's" if t["page"] == codec.ROW_PAGE else "page %d's" % (t["page"] + 1),
        t["control"], "Shift" if t["layer"] else "Normal")


def sent_text(e):
    """An entry of what the mixer sent (P-5)."""
    return "sent %10d: %-8s from %s" % (e["n"], e["msg"].hex(" "),
                                        control_text(e["source"]) if e["source"] else "no control")


def state_text(st):
    where = control_text(st["touched"])
    return ("preset %d page %d%s; bluetooth %s%s; %d touches, the last: %s; storage %s; revision %d, ID %s" % (
        st["preset"] + 1, st["page"] + 1, ", Shift" if st["shift"] else "", st["bluetooth"],
        " (a central)" if st["central"] else "", st["touches"], where, st["store"], st["revision"],
        st["id"].hex() if st["id"] else "none"))


def cmd_state(a):
    import devlink

    with open_device(a) as dev:
        try:
            device_info(dev)
            st = dev.state()
        except devlink.DeviceError as e:
            print("smcconf: %s: %s" % (a.where, e), file=sys.stderr)
            return 1
    print(state_text(st))
    return 0


def cmd_select(a):
    import devlink

    if not 1 <= a.preset <= 4 or (a.page is not None and not 1 <= a.page <= 4):
        print("smcconf: select: the preset and the page are 1-4", file=sys.stderr)
        return 2
    with open_device(a) as dev:
        try:
            device_info(dev)
            dev.select(a.preset - 1, None if a.page is None else a.page - 1)
            give_up = time.monotonic() + 1.0
            while True:
                st = dev.state()
                if st["preset"] == a.preset - 1 and (a.page is None or st["page"] == a.page - 1):
                    break
                if time.monotonic() > give_up:
                    raise devlink.DeviceError("the device did not switch: %s" % state_text(st))
                time.sleep(0.02)
        except devlink.DeviceError as e:
            print("smcconf: %s: %s" % (a.where, e), file=sys.stderr)
            return 1
    print(state_text(st))
    return 0


def cmd_watch(a):
    import devlink

    with open_device(a) as dev:
        try:
            device_info(dev)
            dev.watch(True, sent=a.sent)
            end = time.monotonic() + a.seconds
            while time.monotonic() < end:
                for count, st in dev.events(min(0.2, max(0.0, end - time.monotonic()))):
                    print("event %3d: %s" % (count, state_text(st)), flush=True)
                if a.sent:
                    lost, entries = dev.sent(0)
                    if lost:
                        print("sent: %d not shown (the mixer keeps the last 63)" % lost, flush=True)
                    for e in entries:
                        print(sent_text(e), flush=True)
            dev.watch(False)
        except devlink.DeviceError as e:
            print("smcconf: %s: %s" % (a.where, e), file=sys.stderr)
            return 1
        except KeyboardInterrupt:
            pass
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="smcconf", description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("validate")
    p.add_argument("files", nargs="+")
    p.set_defaults(fn=cmd_validate)
    p = sub.add_parser("format")
    p.add_argument("file")
    g = p.add_mutually_exclusive_group()
    g.add_argument("-o", "--output")
    g.add_argument("--in-place", action="store_true")
    g.add_argument("--check", action="store_true")
    p.set_defaults(fn=cmd_format)
    p = sub.add_parser("encode")
    p.add_argument("file")
    p.add_argument("-o", "--output", required=True)
    p.set_defaults(fn=cmd_encode)
    p = sub.add_parser("decode")
    p.add_argument("file")
    p.add_argument("-o", "--output")
    p.set_defaults(fn=cmd_decode)
    p = sub.add_parser("diff")
    p.add_argument("a")
    p.add_argument("b")
    p.set_defaults(fn=cmd_diff)
    p = sub.add_parser("size")
    p.add_argument("file")
    p.set_defaults(fn=cmd_size)
    for name, fn in (("info", cmd_info), ("pull", cmd_pull), ("push", cmd_push), ("edit", cmd_edit),
                     ("save", cmd_save), ("revert", cmd_revert), ("state", cmd_state), ("select", cmd_select),
                     ("watch", cmd_watch)):
        p = sub.add_parser(name)
        if name == "select":
            p.add_argument("preset", type=int)
            p.add_argument("page", type=int, nargs="?")
        if name == "watch":
            p.add_argument("--seconds", type=float, default=10.0)
            p.add_argument("--sent", action="store_true", help="also what the mixer sends, with the control (P-5)")
        if name in ("push", "edit"):
            p.add_argument("file")
            s = p.add_mutually_exclusive_group()
            s.add_argument("--save", action="store_true", help="save it to flash (SAVE) and wait")
        if name == "push":
            p.add_argument("--no-verify", action="store_true")
            s.add_argument("--wait-save", action="store_true", help="wait for the device's autosave")
        g = p.add_mutually_exclusive_group(required=True)
        g.add_argument("--port", help="the USB CDC port, or auto (found by its USB IDs)")
        g.add_argument("--midi", help="a MIDI port: its name, fd:N or raw:PATH (SysEx)")
        p.add_argument("--timeout", type=float, default=1.0, help="seconds per try (3 tries)")
        if name == "pull":
            p.add_argument("-o", "--output")
            p.add_argument("--binary", action="store_true")
        p.set_defaults(fn=fn)
    a = ap.parse_args(argv)
    if hasattr(a, "midi"):
        a.where = a.port if a.port is not None else "MIDI %s" % a.midi
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())

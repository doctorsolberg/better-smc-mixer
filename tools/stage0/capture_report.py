"""Per-step summary of an `smcmidi session` log (JSON lines).

Usage: python3 -I tools/stage0/capture_report.py LOG.jsonl [--step TEXT] [--full]

For every step attempt it prints the operator's notes and answers and the MIDI received during the
step, grouped by message address (type, channel, number) with timings: what a button sends on
press and release, the value range and resolution of a fader, the value pattern of an encoder.
Messages outside any step and MIDI setup changes (device unplugged, power-cycled) are reported too.
The last section lists every address seen and the steps it appeared in.

A device can send every message on several ports at once (the stock SMC-Mixer firmware does, on
both of its ports). Such mirror sources (the same messages with the same timestamps as another
source) are reported once at the top and left out of every count and statistic.

  --step TEXT   only attempts whose step id contains TEXT
  --full        print every value, also for long groups

Standard library only.
"""

import json
import sys
from collections import Counter, defaultdict

SHORT_GROUP = 16  # groups up to this size are printed message by message


def decode(hex_str):
    """MIDI message bytes → (address, value, label). Address is (kind, channel, number)."""
    b = [int(x, 16) for x in hex_str.split()]
    if not b:
        return ("empty", None, None), None, "empty"
    st = b[0]
    if st >= 0xF0:
        kind = "sysex" if st == 0xF0 else "sys%02X" % st
        return (kind, None, None), len(b), "%s %dB" % (kind, len(b))
    ch = (st & 0x0F) + 1
    hi = st & 0xF0
    d1 = b[1] if len(b) > 1 else 0
    d2 = b[2] if len(b) > 2 else 0
    if hi == 0x90 and d2 > 0:
        return ("note", ch, d1), d2, "on%d" % d2
    if hi in (0x80, 0x90):
        return ("note", ch, d1), 0, "off%d" % d2
    if hi == 0xB0:
        return ("cc", ch, d1), d2, str(d2)
    if hi == 0xE0:
        v = d1 | d2 << 7
        return ("pitch", ch, None), v, str(v)
    if hi == 0xA0:
        return ("poly_at", ch, d1), d2, str(d2)
    if hi == 0xC0:
        return ("pc", ch, None), d1, str(d1)
    return ("chan_at", ch, None), d1, str(d1)


def addr_name(a):
    kind, ch, num = a
    if ch is None:
        return kind
    return "%s ch%d" % (kind, ch) + ("" if num is None else " %d" % num)


class Attempt:
    def __init__(self, step, index, text, t0):
        self.step, self.index, self.text, self.t0 = step, index, text, t0
        self.t1 = None
        self.result = None
        self.notes = []
        self.answers = []
        self.msgs = []  # (t_ns, src, addr, value, label)


class Session:
    def __init__(self):
        self.t_start = None
        self.scenario = None
        self.attempts = []
        self.outside = []  # (after_step, t_ns, src, addr, value, label)
        self.setup = []  # (t_ns, what, name)
        self.mirrors = {}  # mirror source -> (source it repeats, message count)
        self.stop = None
        self.bad_lines = 0


def find_mirrors(recs):
    """Sources whose whole stream (timestamps and bytes) equals that of an earlier source."""
    streams = {}
    for r in recs:
        if r.get("dir") == "rx":
            streams.setdefault(r.get("src", ""), []).append((r["t_ns"], r.get("hex", "")))
    mirrors = {}
    order = list(streams)  # first appearance
    for i, b in enumerate(order):
        for a in order[:i]:
            if a not in mirrors and streams[a] == streams[b]:
                mirrors[b] = (a, len(streams[b]))
                break
    return mirrors


def parse(lines):
    recs = []
    s = Session()
    for line in lines:
        line = line.strip()
        if not line:
            continue
        try:
            r = json.loads(line)
        except ValueError:
            s.bad_lines += 1
            continue
        if isinstance(r, dict) and isinstance(r.get("t_ns"), int):
            recs.append(r)
        else:
            s.bad_lines += 1
    # rx records carry the packet timestamp and can be written after a later marker
    recs.sort(key=lambda r: r["t_ns"])
    if recs:
        s.t_start = recs[0]["t_ns"]
    s.mirrors = find_mirrors(recs)
    cur = None
    for r in recs:
        d, ev, t = r.get("dir"), r.get("event"), r["t_ns"]
        if d == "rx" and r.get("src", "") in s.mirrors:
            continue
        if d == "rx":
            a, v, label = decode(r.get("hex", ""))
            m = (t, r.get("src", ""), a, v, label)
            if cur is not None:
                cur.msgs.append(m)
            else:
                last = s.attempts[-1].step if s.attempts else None
                s.outside.append((last,) + m)
        elif d == "marker" and ev == "step_begin":
            cur = Attempt(r.get("step"), r.get("index"), r.get("text", ""), t)
            s.attempts.append(cur)
        elif d == "marker" and ev == "step_end":
            if cur is not None:
                cur.t1, cur.result = t, r.get("result")
            cur = None
        elif d == "marker" and ev == "note":
            target = cur or (s.attempts[-1] if s.attempts else None)
            if target is not None:
                target.notes.append(r.get("text", ""))
        elif d == "marker" and ev == "answer":
            for a in reversed(s.attempts):
                if a.step == r.get("step"):
                    a.answers.append((r.get("question", ""), r.get("text", "")))
                    break
        elif d == "meta" and ev == "scenario":
            s.scenario = r.get("name")
        elif d == "meta" and ev == "midi_setup" and r.get("what") in ("object_added", "object_removed", "io_error"):
            s.setup.append((t, r["what"], r.get("child", "")))
        elif d == "meta" and ev == "stop":
            s.stop = r.get("how", "stop")
    return s


def group_stats(vals, times):
    """Statistics of a long group of values (a fader or an encoder)."""
    out = []
    distinct = sorted(set(vals))
    out.append("min %d, max %d, %d distinct, first %d, last %d" % (min(vals), max(vals), len(distinct), vals[0], vals[-1]))
    if len(distinct) <= 8:
        hist = Counter(vals)
        out.append("values: " + ", ".join("%d×%d" % (v, hist[v]) for v in distinct))
    else:
        steps = [b - a for a, b in zip(distinct, distinct[1:])]
        out.append("smallest step between distinct values %d, typical %d" % (min(steps), Counter(steps).most_common(1)[0][0]))
    deltas = [b - a for a, b in zip(vals, vals[1:])]
    if deltas:
        repeats = sum(1 for x in deltas if x == 0)
        turns = sum(1 for x, y in zip([d for d in deltas if d], [d for d in deltas if d][1:]) if (x > 0) != (y > 0))
        out.append("largest jump %d, direction changes %d, repeated values %d" % (max(abs(x) for x in deltas), turns, repeats))
    gaps = [(b - a) / 1e6 for a, b in zip(times, times[1:])]
    if gaps:
        span = (times[-1] - times[0]) / 1e9
        rate = (len(times) - 1) / span if span > 0 else 0
        out.append("gap min %.1f ms, median %.1f ms; %.0f msg/s over %.2f s" % (min(gaps), sorted(gaps)[len(gaps) // 2], rate, span))
    return out


def print_attempt(a, full, out):
    dur = "" if a.t1 is None else ", %.1f s" % ((a.t1 - a.t0) / 1e9)
    out.append("")
    out.append("[%s] %s (%s%s, %d msgs)" % (a.index, a.step, a.result or "open", dur, len(a.msgs)))
    out.append("    " + a.text)
    for n in a.notes:
        out.append("    note: " + n)
    for q, ans in a.answers:
        out.append("    Q: %s  A: %s" % (q, ans or "—"))
    groups = defaultdict(list)
    srcs = Counter()
    for m in a.msgs:
        groups[m[2]].append(m)
        srcs[m[1]] += 1
    if len(srcs) > 1:
        out.append("    sources: " + ", ".join("%s ×%d" % kv for kv in srcs.most_common()))
    for addr in sorted(groups, key=lambda k: groups[k][0][0]):
        g = groups[addr]
        head = "    %s ×%d" % (addr_name(addr), len(g))
        if len(g) <= SHORT_GROUP or full:
            seq = ", ".join("+%.3f %s" % ((m[0] - a.t0) / 1e9, m[4]) for m in g)
            out.append(head + ": " + seq)
        else:
            out.append(head + ", from +%.3f s:" % ((g[0][0] - a.t0) / 1e9))
            vals = [m[3] for m in g if m[3] is not None]
            if vals:
                for line in group_stats(vals, [m[0] for m in g]):
                    out.append("        " + line)


def report(s, step_filter=None, full=False):
    out = []
    t0 = s.t_start or 0
    out.append("scenario: %s; %d step attempts; ended: %s" % (s.scenario or "?", len(s.attempts), s.stop or "no stop record"))
    if s.bad_lines:
        out.append("unreadable lines: %d" % s.bad_lines)
    for b, (a, n) in s.mirrors.items():
        out.append("mirror source left out: %s repeats %s (all %d msgs, same timestamps)" % (b, a, n))
    for t, what, name in s.setup:
        out.append("midi setup at +%.1f s: %s %s" % ((t - t0) / 1e9, what, name))
    for a in s.attempts:
        if step_filter is None or step_filter in (a.step or ""):
            print_attempt(a, full, out)
    if s.outside and step_filter is None:
        out.append("")
        out.append("outside steps: %d msgs" % len(s.outside))
        by = defaultdict(list)
        for m in s.outside:
            by[(m[0], m[3])].append(m)
        for (after, addr), g in by.items():
            out.append("    after %s: %s ×%d, values %s" % (after or "start", addr_name(addr), len(g), " ".join(m[5] for m in g[:12]) + (" …" if len(g) > 12 else "")))
    if step_filter is None:
        seen = defaultdict(Counter)
        for a in s.attempts:
            for m in a.msgs:
                seen[m[2]][a.step] += 1
        if seen:
            out.append("")
            out.append("addresses seen:")
            for addr in sorted(seen, key=lambda k: (k[0], k[1] or 0, k[2] if k[2] is not None else -1)):
                out.append("    %-18s %s" % (addr_name(addr), ", ".join("%s ×%d" % kv for kv in seen[addr].items())))
    return out


def main(argv):
    args = list(argv)
    full = "--full" in args
    if full:
        args.remove("--full")
    step_filter = None
    if "--step" in args:
        i = args.index("--step")
        if i + 1 >= len(args):
            sys.exit("--step needs a value")
        step_filter = args[i + 1]
        del args[i:i + 2]
    if len(args) != 1:
        sys.exit(__doc__.strip())
    with open(args[0], encoding="utf-8") as f:
        s = parse(f)
    print("\n".join(report(s, step_filter, full)))


if __name__ == "__main__":
    main(sys.argv[1:])

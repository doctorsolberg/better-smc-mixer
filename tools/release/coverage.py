#!/usr/bin/env python3
"""Line coverage of the firmware's portable code by the host tests (night plan N-12; docs/build.md,
"Checks and tests"): `make coverage` builds every C test and the fuzzer with --coverage into
build/host-cov, runs them, then this script merges the counters of every program (gcov -t on each
.gcda; the compiler's own tool: Apple's gcov is llvm-cov, Linux's is gcc's) and prints each source
file's covered lines.

  python3 -I tools/release/coverage.py build/host-cov [--uncovered FILE...] [--report OUT]

--uncovered lists the lines no test reached in the named sources (as in the table: config/cfgbin.c),
with their text; --report writes the whole list for every file. Run from the repository root or
anywhere: the sources are found under firmware/. Standard library only.
"""
import argparse
import collections
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FW = os.path.join(ROOT, "firmware")
# what is measured: the firmware's own portable code, not the tests, the host ports or the simulator
AREAS = ("core/", "config/", "link/", "ctl/", "input/", "midi/", "lib/smclog/", "metrics/", "diag/")
LINE = re.compile(r"^\s*([^:]+):\s*(\d+):(.*)$")


def gcov_tool():
    for t in (os.environ.get("GCOV"), "gcov", "llvm-cov"):
        if t and shutil.which(t):
            return [t, "gcov"] if os.path.basename(t) == "llvm-cov" else [t]
    raise SystemExit("coverage.py: no gcov (the compiler's coverage tool)")


def collect(build):
    """{source: {line: (count, text)}} merged over every program's counters."""
    tool = gcov_tool()
    lines = collections.defaultdict(dict)
    gcdas = sorted(f for f in os.listdir(build) if f.endswith(".gcda"))
    if not gcdas:
        raise SystemExit("coverage.py: no .gcda in %s: run make coverage" % build)
    for g in gcdas:
        r = subprocess.run(tool + ["-t", "-o", build, os.path.join(build, g)], cwd=FW,
                           capture_output=True, text=True, errors="replace")
        src = None
        for raw in r.stdout.splitlines():
            m = LINE.match(raw)
            if not m:
                continue
            count, no, text = m.group(1).strip(), int(m.group(2)), m.group(3)
            if no == 0:
                if text.startswith("Source:"):
                    src = os.path.normpath(text[len("Source:"):])
                    if os.path.isabs(src):
                        src = os.path.relpath(src, FW)
                continue
            if src is None or count == "-":
                continue
            n = 0 if count in ("#####", "=====") else int(count.rstrip("*"))
            old = lines[src].get(no, (0, text))[0]
            lines[src][no] = (old + n, text)
    return lines


def measured(src):
    return src.startswith(AREAS) and not src.startswith("tests/")


def ranges(nums):
    out, start, prev = [], None, None
    for n in nums:
        if start is None:
            start = prev = n
        elif n == prev + 1:
            prev = n
        else:
            out.append((start, prev))
            start = prev = n
    if start is not None:
        out.append((start, prev))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("build")
    ap.add_argument("--uncovered", nargs="*", default=[])
    ap.add_argument("--report")
    a = ap.parse_args(argv)
    lines = collect(os.path.abspath(a.build))
    total_c = total_n = 0
    rows = []
    for src in sorted(s for s in lines if measured(s)):
        ls = lines[src]
        c = sum(1 for n, _ in ls.values() if n)
        rows.append((src, c, len(ls)))
        total_c += c
        total_n += len(ls)
    print("%-34s %6s %6s %6s" % ("source (firmware/)", "lines", "hit", "%"))
    for src, c, n in rows:
        print("%-34s %6d %6d %5.1f%%" % (src, n, c, 100.0 * c / n if n else 100))
    print("%-34s %6d %6d %5.1f%%" % ("all", total_n, total_c, 100.0 * total_c / total_n if total_n else 100))
    want = set(a.uncovered)
    out = open(a.report, "w", encoding="utf-8") if a.report else None
    for src, _, _ in rows:
        miss = sorted(no for no, (n, _) in lines[src].items() if n == 0)
        if not miss or (src not in want and not out):
            continue
        text = ["%s: %d lines not reached" % (src, len(miss))]
        for s, e in ranges(miss):
            for no in range(s, e + 1):
                text.append("  %5d  %s" % (no, lines[src][no][1].rstrip()))
            text.append("")
        if src in want:
            print("\n".join(text))
        if out:
            out.write("\n".join(text) + "\n")
    if out:
        out.close()
        print("coverage: every line not reached is in %s" % a.report)
    return 0


if __name__ == "__main__":
    sys.exit(main())

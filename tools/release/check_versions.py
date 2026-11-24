#!/usr/bin/env python3
"""Every version number of the product agrees on both sides (SPEC Q-29; docs/config.md, "Versions and
migrations"; docs/app.md, "One product").

The firmware and the editor run on different devices and must speak the same formats: the file's
schema, the binary form, the configuration protocol, the command link and the log format. Each
version is written down in several places (C headers, the editor's JavaScript, the Python tools, the
recorded transcripts); this check reads them all and fails when one differs from the others.

  python3 -I tools/release/check_versions.py         make quick runs it; exit 1 on a mismatch

The source of each version is the first place listed for it; a change to a version changes every
place in one step. Standard library only.
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def _json(path, *keys):
    def get(text):
        v = json.loads(text)
        for k in keys:
            v = v[k]
        return [v]
    return path, get


def _re(path, pattern, every=False):
    rx = re.compile(pattern, re.M)

    def get(text):
        found = [int(m.group(1)) for m in rx.finditer(text)]
        return found if every else found[:1]
    return path, get


def _text(path):
    def get(text):
        return [text.strip()]
    return path, get


def _semver_c(path, prefix):
    """MAJOR.MINOR.PATCH from three C defines PREFIX_MAJOR, _MINOR, _PATCH."""
    def get(text):
        parts = []
        for k in ("MAJOR", "MINOR", "PATCH"):
            m = re.search(r"^#define %s_%s (\d+)\s*$" % (prefix, k), text, re.M)
            if not m:
                return []
            parts.append(m.group(1))
        return [".".join(parts)]
    return path, get


def _re_text(path, pattern):
    rx = re.compile(pattern, re.M)

    def get(text):
        return [m.group(1) for m in rx.finditer(text)][:1]
    return path, get


# version -> the places that hold it, the source first
PLACES = {
    # one semantic version for the whole product (the owner, 2026-11-14; docs/build.md, "The version")
    "the product": [
        _text("VERSION"),
        _semver_c("firmware/app/include/app_version.h", "APP_VERSION"),
        _re_text("app/editor/js/version.js", r"^export const VERSION = '([^']+)';"),
    ],
    "the file's schema": [
        _json("config/schema.json", "schemaVersion"),
        _re("firmware/config/cfg_schema.h", r"^#define CFG_SCHEMA_VERSION (\d+)"),
        _json("config/smc-config.schema.json", "properties", "schemaVersion", "const"),
        _json("config/factory.json", "schemaVersion"),
        _json("config/test.json", "schemaVersion"),
        _re("app/editor/js/templates.js", r"schemaVersion: (\d+)", every=True),
    ],
    "the binary form": [
        _json("config/schema.json", "binaryVersion"),
        _re("firmware/config/cfg_schema.h", r"^#define CFG_BINARY_VERSION (\d+)"),
    ],
    "the configuration protocol": [
        _re("firmware/link/cfglink.h", r"^#define CFGLINK_VERSION (\d+)"),
        _re("app/editor/js/link.js", r"^export const CFG_VERSION = (\d+);"),
        _re("tools/smcconf/devlink.py", r"^VERSION = (\d+)"),
        _re("tools/editortest/transcripts.py", r'"cfg_version": (\d+)'),
        _json("tools/editortest/vectors/link-v1.json", "about", "cfg_version"),
    ],
    "the command link": [
        _re("firmware/link/link.h", r"^#define LINK_VERSION (\d+)"),
        _re("app/editor/js/link.js", r"^export const LINK_VERSION = (\d+);"),
        _re("tools/smclog/smclog.py", r"^LINK_VERSION = (\d+)"),
        _re("tools/editortest/transcripts.py", r'"link_version": (\d+)'),
        _json("tools/editortest/vectors/link-v1.json", "about", "link_version"),
    ],
    "the log format": [
        _re("firmware/lib/smclog/smclog.h", r"^#define SMCLOG_FORMAT_VERSION (\d+)"),
        _re("tools/smclog/smclog.py", r"^FORMAT_VERSION = (\d+)"),
    ],
}


def read_file(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return f.read()


def check(read=read_file):
    """Problems as text lines (empty: every version agrees). read(path) gives a file's text."""
    problems = []
    for what, places in PLACES.items():
        source = None
        for path, get in places:
            try:
                values = get(read(path))
            except (OSError, ValueError, KeyError, TypeError) as e:
                problems.append("%s: %s: not read (%s)" % (what, path, e))
                continue
            if not values:
                problems.append("%s: %s: the version is not found (the pattern is out of date?)" % (what, path))
                continue
            if source is None:
                source = (path, values[0])
            for v in values:
                if v != source[1]:
                    problems.append("%s: %s says %s, %s says %s" % (what, path, v, source[0], source[1]))
    return problems


def main():
    problems = check()
    for p in problems:
        print("check_versions: " + p)
    n = sum(len(p) for p in PLACES.values())
    print("check_versions: %d versions in %d places, %d mismatches" % (len(PLACES), n, len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())

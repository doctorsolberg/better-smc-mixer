"""Tests of check_docs.py on a planted tree, then on the repository. Standard library only."""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_docs  # noqa: E402

FAILS = 0


def check(cond, what):
    global FAILS
    if not cond:
        FAILS += 1
        print("FAIL: " + what)


def write(root, rel, text):
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


def main():
    with tempfile.TemporaryDirectory() as root:
        write(root, "docs/b.md", "# B\n\n## The CPU clock\n\n## I-6: live edits (2026-11-13)\n\n"
                                 "## On the device: the first run\n\n**Autosave.** Text.\n\n"
                                 "```\n## Not a heading\n```\n")
        write(root, "src/x.c", "int x;\n")
        good = ("# A\n\n[b](b.md), \"The CPU clock\"; [b](b.md), \"I-6\"; [b](b.md), \"the first run\";\n"
                "[b](b.md), \"Autosave\", \"The CPU clock\"; [x](../src/x.c:3); [b](b.md#the-cpu-clock);\n"
                "[web](https://example.com); `[code](missing.md)`\n")
        write(root, "docs/a.md", good)
        check(check_docs.check(root) == [], "good links pass: %r" % check_docs.check(root))

        write(root, "docs/a.md", good + "[c](c.md)\n[b](b.md), \"Not a heading\"\n[b](b.md), \"The CPU\"\n"
                                        "[b](b.md#nowhere)\n")
        found = check_docs.check(root)
        msgs = sorted(m for _f, _l, m in found)
        want = sorted(["no such file: c.md", "no section \"Not a heading\" in b.md",
                       "no section \"The CPU\" in b.md", "no heading for #nowhere in b.md"])
        check(msgs == want, "broken links found: %r" % msgs)
        check(all(f == "docs/a.md" and l >= 4 for f, l, _m in found), "lines: %r" % found)

        write(root, "docs/a.md", "# Lines\n```\n[example](missing-example.md)\n```\n`[inline](missing-inline.md)`\n[real](missing-real.md)\n")
        found = check_docs.check(root)
        check(found == [("docs/a.md", 6, "no such file: missing-real.md")],
              "the actual line after masked examples: %r" % found)

    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    for rel, line, msg in check_docs.check(root):
        check(False, "the repository: %s:%d: %s" % (rel, line, msg))

    print("test_check_docs: %s" % ("FAILED (%d)" % FAILS if FAILS else "passed"))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

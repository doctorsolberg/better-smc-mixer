"""Tests of check_public.py: one planted sample of each kind is found, clean files pass.

The samples are put together at run time, so this file holds none of them itself (it is published
and checked too). Standard library only.
"""
import os
import random
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_public  # noqa: E402

FAILS = 0


def check(cond, what):
    global FAILS
    if not cond:
        FAILS += 1
        print("FAIL: " + what)


def write(root, rel, data):
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(data if isinstance(data, bytes) else data.encode("utf-8"))


def kinds(root):
    findings, notes = check_public.check(root)
    return {(rel, kind) for rel, kind, _line, _detail in findings}, notes


def main():
    rnd = random.Random(7)
    stock = bytes(rnd.randrange(256) for _ in range(4096))
    secret = "SN-" + "".join(rnd.choice("ABCDEFGHJK0123456789") for _ in range(10))
    with tempfile.TemporaryDirectory() as root:
        write(root, ".gitignore", "/materials/\n/build/\n.DS_Store\n__pycache__/\n")
        write(root, "materials/dumps/stock-a.bin", stock)
        write(root, "materials/dumps/rom-read-ours.bin", b"OURS" * 64 + stock[:8])
        write(root, "materials/private-strings.txt", "# local\n%s\nzedq\n!pay.example/zedq\n" % secret)
        write(root, "build/out.bin", b"\0" * 64)
        write(root, "tools/x/__pycache__/m.pyc", b"\0\1\2")
        write(root, ".DS_Store", b"\0\1")

        write(root, "clean.md", "# Clean\n\nNothing to see: noreply" + "@example.com, 00:00:00:00:00:00.\n")
        write(root, "a/binary.dat", b"\0\1\2\3")
        write(root, "site/img/photo.webp", b"RIFF\0\0\0\0WEBPVP8 ")       # a site image: allowed
        write(root, "site/img/notes.bin", b"\0\1\2\3")                    # not an image: binary
        write(root, "a/photo.webp", b"RIFF\0\0\0\0WEBPVP8 ")             # an image elsewhere: binary
        write(root, "site/img/stock.png", b"\x89PNG" + stock[500:600])     # stock bytes are found anyway
        write(root, "a/stock.dat", b"\0\0" + stock[1000:1100] + b"\0")
        write(root, "a/stock-hex.c", "static const unsigned char t[] = {%s};\n"
              % ", ".join("0x%02x" % b for b in stock[2000:2064]))
        write(root, "a/stock-str.json", '{"hex": "%s"}\n' % stock[3000:3060].hex())
        write(root, "a/large.txt", "x" * (check_public.LARGE + 1))
        write(root, "a/private.md", "the serial is %s\n" % secret.lower())
        write(root, "a/allowed.md", "tip jar: https://PAY.example/ZedQ\n")
        write(root, "a/allowed-and-private.md", "https://pay.example/zedq and zedq's phone\n")
        write(root, "a/home.md", "see /Us" + "ers/alice/project\n")
        write(root, "a/email.md", "write to a.b" + "@" + "corp.io\n")
        write(root, "a/mac.md", "address " + ":".join(["a4", "c1", "38", "0f", "12", "9e"]) + "\n")
        write(root, "a/language.md", "text " + chr(0x0416) + "\n")
        write(root, "a/names.md", "like the E" + "C4\n")
        write(root, "third_party/lib/AUTHORS", "Some One <some" + "@" + "one.org>, /ho" + "me/someone\n")
        write(root, "a/near.md", "UUID 03B80E5A-EDE8-4B33-A751-6CE34" + "EC4C700, hash 38de54351d81878e6543f2b022d\n")

        found, notes = kinds(root)
        want = {("a/binary.dat", "binary"), ("a/stock.dat", "binary"), ("a/stock.dat", "stock"),
                ("a/stock-hex.c", "stock"), ("a/stock-str.json", "stock"), ("a/large.txt", "large"),
                ("a/private.md", "private"), ("a/home.md", "home"), ("a/email.md", "email"),
                ("a/mac.md", "mac"), ("a/language.md", "language"), ("a/names.md", "names"),
                ("a/allowed-and-private.md", "private"), ("site/img/notes.bin", "binary"), ("a/photo.webp", "binary"),
                ("site/img/stock.png", "stock")}
        for w in sorted(want - found):
            check(False, "not found: %s %s" % w)
        for f in sorted(found - want):
            check(False, "found and not planted: %s %s" % f)
        check(notes == [], "notes with every input present: %r" % notes)

        published = set(check_public.published(root))
        for rel in (".DS_Store", "build/out.bin", "materials/dumps/stock-a.bin", "tools/x/__pycache__/m.pyc"):
            check(rel not in published, "ignored file published: " + rel)
        check("clean.md" in published and "third_party/lib/AUTHORS" in published, "published files listed")

        # Without the local materials the checks that need them are skipped, and say so.
        os.remove(os.path.join(root, "materials", "dumps", "stock-a.bin"))
        os.remove(os.path.join(root, "materials", "private-strings.txt"))
        found, notes = kinds(root)
        check(("a/stock-hex.c", "stock") not in found and ("a/private.md", "private") not in found,
              "stock and private checks skipped without materials")
        check(len(notes) == 2, "two notes without materials: %r" % notes)

    # The repository itself.
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    findings, _notes = check_public.check(root)
    for rel, kind, line, detail in findings:
        check(False, "the repository: %s:%d %s %s" % (rel, line, kind, detail if kind != "private" else ""))

    print("test_check_public: %s" % ("FAILED (%d)" % FAILS if FAILS else "passed"))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

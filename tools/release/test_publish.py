"""Host tests of publish.py on a synthetic package: no images built, no network."""
import contextlib
import hashlib
import io
import json
import os
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import publish  # noqa: E402


def package(root, version="0.1.0", sha="a" * 64):
    os.makedirs(os.path.join(root, "editor"))
    with open(os.path.join(root, "editor", "index.html"), "w") as f:
        f.write("<!doctype html>\n")
    with open(os.path.join(root, "manifest.json"), "w") as f:
        json.dump({"product": "better-smc-mixer", "version": version, "images": {
            "controller-ble": {"build_id": "b" * 40, "sha256": sha, "size": 10}}}, f)
    return root


def quiet(fn, *a):
    with contextlib.redirect_stdout(io.StringIO()) as out:
        r = fn(*a)
    return r, out.getvalue()


class Publish(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.pkg = package(os.path.join(self.tmp.name, "pkg"))

    def test_expected_matches_recorded_and_refuses_another_build(self):
        exp = os.path.join(self.tmp.name, "expected.json")
        self.assertEqual(quiet(publish.record, self.pkg, exp)[0], 0)
        self.assertEqual(quiet(publish.expected, self.pkg, exp)[0], 0)
        other = package(os.path.join(self.tmp.name, "other"), sha="c" * 64)
        r, out = quiet(publish.expected, other, exp)
        self.assertEqual(r, 1)
        self.assertIn("verified on the mixer", out)
        newer = package(os.path.join(self.tmp.name, "newer"), version="0.1.1")
        self.assertEqual(quiet(publish.expected, newer, exp)[0], 1)

    def test_zip_is_deterministic_with_one_top_folder(self):
        a, b = (os.path.join(self.tmp.name, n) for n in ("a.zip", "b.zip"))
        quiet(publish.make_zip, self.pkg, a)
        quiet(publish.make_zip, self.pkg, b)
        with open(a, "rb") as fa, open(b, "rb") as fb:
            self.assertEqual(hashlib.sha256(fa.read()).digest(), hashlib.sha256(fb.read()).digest())
        with zipfile.ZipFile(a) as z:
            self.assertEqual(sorted(z.namelist()), ["better-smc-mixer-0.1.0/editor/index.html",
                                                    "better-smc-mixer-0.1.0/manifest.json"])

    def test_site_puts_package_at_root_and_older_releases_under_versions(self):
        old = package(os.path.join(self.tmp.name, "old"), version="0.0.9")
        old_zip = os.path.join(self.tmp.name, "old.zip")
        same_zip = os.path.join(self.tmp.name, "same.zip")
        quiet(publish.make_zip, old, old_zip)
        quiet(publish.make_zip, self.pkg, same_zip)
        src = os.path.join(self.tmp.name, "site-src")
        os.makedirs(src)
        with open(os.path.join(src, "index.html"), "w") as f:
            f.write("home\n")
        out = os.path.join(self.tmp.name, "site")
        quiet(publish.site, self.pkg, src, out, [old_zip, same_zip])
        for p in ("index.html", "manifest.json", "editor/index.html", ".nojekyll",
                  "v0.0.9/manifest.json", "v0.0.9/editor/index.html"):
            self.assertTrue(os.path.isfile(os.path.join(out, p)), p)
        self.assertFalse(os.path.exists(os.path.join(out, "v0.1.0")))
        with open(os.path.join(out, "index.html")) as f:
            self.assertEqual(f.read(), "home\n")

    def test_site_refuses_a_zip_that_leaves_its_folder(self):
        bad = os.path.join(self.tmp.name, "bad.zip")
        with zipfile.ZipFile(bad, "w") as z:
            z.writestr("better-smc-mixer-0.0.1/../../evil", "x")
        src = os.path.join(self.tmp.name, "s")
        os.makedirs(src)
        with self.assertRaises(SystemExit):
            quiet(publish.site, self.pkg, src, os.path.join(self.tmp.name, "out"), [bad])

    def test_notes_fill_version_and_image_table(self):
        md = os.path.join(self.tmp.name, "notes.md")
        with open(md, "w") as f:
            f.write("# {version}\n\n{images}\n")
        _, out = quiet(publish.notes, self.pkg, md)
        self.assertIn("# 0.1.0", out)
        self.assertIn("| `controller-ble` | 10 | `bbbbbbbbbbbb` | `" + "a" * 64 + "` |", out)


if __name__ == "__main__":
    unittest.main()

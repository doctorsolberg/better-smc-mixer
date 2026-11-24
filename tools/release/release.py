#!/usr/bin/env python3
"""A release package (docs/app.md, "Hosting on GitHub Pages"; docs/build.md, "Release packages"):
the product's firmware images, the editor as static files and the manifest, into build/release/.
Nothing is published by this script, nothing talks to a device, nothing is downloaded.

  python3 -I tools/release/release.py [--no-build] [--twice] [--out DIR] [--date YYYY-MM-DD]

  --no-build   pack the images already in build/ (else every image is built first, in Docker)
  --twice      build every image again from scratch (make -B) and compare the bytes with the
               first build: a release must be reproducible; the differences are named
  --out DIR    the package's directory (default build/release)
  --date D     the publication date: the manifest carries it and "dry": false (the release
               workflow sets it); without it the package is a dry one (date null, "dry": true)

The package:

  firmware/<image>.bin              the bootloader, the controller (USB), the controller with
                                    BLE-MIDI, the hardware test, as build_app.sh / build_boot.sh cut
                                    them; the editor installs the controller with BLE-MIDI over
                                    M-Vave's firmware (SPEC Q-18; docs/app.md, "Firmware")
  firmware/cfg_tool.bin             the SDK's default settings, which that controller needs beside
                                    it (tools/image/installplan.py, --cfg); manifest "install"
                                    names the two
  firmware/<image>.smclog.json      each image's log dictionary (LOG-22: tied to its build ID)
  editor/                           app/editor's page, styles and modules, with data/ holding
                                    config/schema.json and config/factory.json (the page reads
                                    data/ when ../../config/ is not there)
  guide/                            docs/user/ (drafts), links rewritten for this package
  reference/                        public project Markdown and linked text assets; source-only
                                    code references are labelled, never copied from private trees
  NOTICE                            what the images hold from others
  LICENSE                           ours (MIT)
  licenses/                         the texts of the licenses NOTICE names (newlib, compiler-rt,
                                    TinyUSB, the JieLi SDK)
  manifest.json                     the versions, each file's size and SHA-256, the build IDs
  SHA256SUMS                        the same hashes, for sha256sum -c

Then the publication check (tools/release/check_public.py) runs on what was packed: the images
for stock firmware bytes and the private strings, the text files for everything it checks. Exit 1
when that check or the reproducibility comparison finds something, 2 when a build fails.
Standard library only.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_public  # noqa: E402
import check_docs  # noqa: E402
import docbundle  # noqa: E402

# image -> (the build environment, the build directory, the binary there, the build script)
IMAGES = {
    "boot": ({}, "build/boot", "boot.bin", "tools/env/build_boot.sh"),
    "controller": ({}, "build/app", "app.bin", "tools/env/build_app.sh"),
    "controller-ble": ({"BLE": "1"}, "build/app-ble", "app.bin", "tools/env/build_app.sh"),
    "hwtest": ({"VARIANT": "hwtest"}, "build/hwtest", "app.bin", "tools/env/build_app.sh"),
}
EDITOR_FILES = ("index.html", "app.css")
# What the editor installs (docs/app.md, "Firmware"; SPEC Q-18): the controller with Bluetooth, and
# the SDK's default settings file our SDK build needs beside it (installplan.py, --cfg).
INSTALL_IMAGE = "controller-ble"
CFG_TOOL = "deps/fw-AC63_BT_SDK/cpu/br25/tools/cfg_tool.bin"
EDITOR_DATA = ("config/schema.json", "config/factory.json")
THIRD_PARTY_LICENSES = (("licenses/newlib.txt", "licenses/newlib.txt"),
                        ("licenses/compiler-rt.txt", "licenses/compiler-rt.txt"),
                        ("third_party/tinyusb/LICENSE", "licenses/tinyusb.txt"),
                        ("deps/fw-AC63_BT_SDK/LICENSE", "licenses/jieli-ac63-sdk.txt"))


def sha256(path):
    with open(os.path.join(ROOT, path) if not os.path.isabs(path) else path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def build(force=False):
    """Builds every image; True when all built."""
    for name, (env, _, _, script) in IMAGES.items():
        e = dict(os.environ)
        for flag in ("VARIANT", "BLE", "BOOTMARK"):
            e.pop(flag, None)
        e.update(env)
        print("release: building %s%s" % (name, " from scratch" if force else ""), flush=True)
        r = subprocess.run([os.path.join(ROOT, script)] + (["-B"] if force else []), cwd=ROOT, env=e,
                           stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        if r.returncode:
            print("release: the build of %s failed (exit %d): see logs/build/" % (name, r.returncode), file=sys.stderr)
            return False
    return True


def images():
    """{image: {bin, dict, sha256, build_id}} of what build/ holds now."""
    out = {}
    for name, (_, d, binary, _) in IMAGES.items():
        b = os.path.join(d, binary)
        dic = os.path.join(d, os.path.splitext(binary)[0] + ".smclog.json")
        if not os.path.exists(os.path.join(ROOT, b)):
            raise SystemExit("release: no %s: build it first (or drop --no-build)" % b)
        with open(os.path.join(ROOT, dic), encoding="utf-8") as f:
            build_id = json.load(f)["build_id"]
        out[name] = {"bin": b, "dict": dic, "sha256": sha256(b), "dict_sha256": sha256(dic), "build_id": build_id}
    return out


def product_version():
    """The product's one version (VERSION; docs/build.md, "The version")."""
    with open(os.path.join(ROOT, "VERSION"), encoding="ascii") as f:
        return f.read().strip()


def versions():
    with open(os.path.join(ROOT, "config", "schema.json"), encoding="utf-8") as f:
        s = json.load(f)

    def define(path, name):
        with open(os.path.join(ROOT, path), encoding="utf-8") as f:
            return int(re.search(r"^#define %s (\d+)" % name, f.read(), re.M).group(1))
    return {"schema": s["schemaVersion"], "binary": s["binaryVersion"],
            "configuration_protocol": define("firmware/link/cfglink.h", "CFGLINK_VERSION"),
            "link": define("firmware/link/link.h", "LINK_VERSION"),
            "log_format": define("firmware/lib/smclog/smclog.h", "SMCLOG_FORMAT_VERSION")}


def copy(src, out, rel):
    dst = os.path.join(out, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(os.path.join(ROOT, src), dst)
    return rel


def validate_output(out):
    """Never delete the checkout, its parents, or an unrelated nonempty directory."""
    out = os.path.abspath(out)
    real, root = os.path.realpath(out), os.path.realpath(ROOT)
    if docbundle.inside(root, real) or os.path.islink(out):
        raise ValueError("release output must not be the checkout, its parent, or a symlink")
    if os.path.exists(out) and not os.path.isdir(out):
        raise ValueError("release output is not a directory")
    if os.path.isdir(out) and os.listdir(out):
        try:
            with open(os.path.join(out, "manifest.json"), encoding="utf8") as f:
                previous = json.load(f)
        except (OSError, ValueError):
            previous = {}
        if (not isinstance(previous, dict) or previous.get("product") != "better-smc-mixer"
                or not isinstance(previous.get("dry"), bool)):
            raise ValueError("refusing to replace a non-release directory: " + out)
    return out


def pack(out, imgs, date=None):
    """Assemble in a sibling workspace; keep a previous package if assembly fails."""
    out = validate_output(out)
    parent = os.path.dirname(out)
    os.makedirs(parent, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".smc-release-", dir=parent) as work:
        assembled = os.path.join(work, "package")
        files = _pack_contents(assembled, imgs, date)
        previous = None
        if os.path.exists(out):
            previous = tempfile.mkdtemp(prefix=".smc-release-previous-", dir=parent)
            os.rmdir(previous)
            os.replace(out, previous)
        try:
            os.replace(assembled, out)
        except OSError:
            if previous is not None:
                try:
                    os.replace(previous, out)
                except OSError as error:
                    raise ValueError("package replacement failed; previous package kept at " + previous) from error
            raise
        if previous is not None:
            try:
                shutil.rmtree(previous)
            except OSError:
                print("release: note: previous package kept at " + previous, file=sys.stderr)
    return files


def _pack_contents(out, imgs, date=None):
    """Write one complete staged package; all names returned are relative."""
    os.makedirs(out)
    files, aliases = [], {}

    def put(src, dst):
        files.append(copy(src, out, dst))
        aliases[src] = dst

    for name, i in imgs.items():
        put(i["bin"], "firmware/%s.bin" % name)
        put(i["dict"], "firmware/%s.smclog.json" % name)
    installs = INSTALL_IMAGE in imgs and os.path.exists(os.path.join(ROOT, CFG_TOOL))
    if installs:
        put(CFG_TOOL, "firmware/cfg_tool.bin")
    editor_dir = os.path.join("app", "editor")
    for f in EDITOR_FILES:
        put(os.path.join(editor_dir, f), "editor/" + f)
    for f in sorted(os.listdir(os.path.join(ROOT, editor_dir, "js"))):
        if f.endswith(".js"):
            put(os.path.join(editor_dir, "js", f), "editor/js/" + f)
    aliases[editor_dir] = "editor"
    for f in EDITOR_DATA:
        put(f, "editor/data/" + os.path.basename(f))
    put("NOTICE", "NOTICE")
    for lic in ("LICENSE", "LICENSE.md", "LICENSE.txt"):
        if os.path.exists(os.path.join(ROOT, lic)):
            put(lic, lic)
    # the licenses NOTICE names, as their texts (Apache 2.0 section 4a: a copy goes with the work)
    for src, dst in THIRD_PARTY_LICENSES:
        if os.path.exists(os.path.join(ROOT, src)):
            put(src, dst)
    files.extend(docbundle.bundle(ROOT, out, aliases))
    broken = check_docs.check(out)
    if broken:
        raise ValueError("packaged documentation has broken links: %s:%d: %s" % broken[0])
    manifest = {
        "product": "better-smc-mixer",
        # the product's version (the firmware and the editor carry the same); the date is the
        # publication's (--date), which a dry package does not have
        "version": product_version(),
        "date": date,
        "dry": date is None,
        "versions": versions(),
        # compatibility goes by the format and protocol numbers above (check_versions.py), not by
        # this number: the editor in this package is the one made for these images
        "editor": product_version(),
        # what the editor writes over M-Vave's firmware (js/stocklink.js, install())
        "install": {"app": "firmware/%s.bin" % INSTALL_IMAGE, "cfg": "firmware/cfg_tool.bin"} if installs else None,
        "images": {n: {"file": "firmware/%s.bin" % n, "size": os.path.getsize(os.path.join(out, "firmware/%s.bin" % n)),
                       "sha256": i["sha256"], "build_id": i["build_id"],
                       "log_dictionary": "firmware/%s.smclog.json" % n} for n, i in imgs.items()},
        "files": {f: {"size": os.path.getsize(os.path.join(out, f)), "sha256": sha256(os.path.join(out, f))}
                  for f in sorted(files)},
    }
    with open(os.path.join(out, "manifest.json"), "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=1, sort_keys=True)
        fh.write("\n")
    with open(os.path.join(out, "SHA256SUMS"), "w", encoding="utf-8") as fh:
        for f in sorted(files + ["manifest.json"]):
            fh.write("%s  %s\n" % (sha256(os.path.join(out, f)), f))
    return sorted(files + ["manifest.json", "SHA256SUMS"])


def scan(out, files):
    """check_public on the package: (findings [(file, kind, line, detail)], notes). An image is
    binary by design, so for it only the private strings are findings. Its runs of stock firmware
    bytes are a note: the JieLi SDK and the toolchain's runtime (compiler-rt) are in the stock
    firmware and in ours alike (docs/build.md, "Release packages"), which is a licence question
    (NOTICE), not vendor data copied; a count per image keeps a change visible."""
    stock = check_public.load_stock(ROOT)
    private = check_public.load_private(ROOT)
    found, notes = [], []
    if stock is None:
        notes.append("no stock firmware dumps in materials/: the stock-bytes check was skipped")
    if private is None:
        notes.append("no materials/private-strings.txt: the private-strings check was skipped")
    for rel in files:
        with open(os.path.join(out, rel), "rb") as fh:
            data = fh.read()
        image = rel.startswith("firmware/") and rel.endswith(".bin")
        runs = 0
        for kind, line, detail in check_public.check_file(rel, data, stock, private):
            if image and kind in ("binary", "large"):
                continue
            if image and kind == "stock":
                runs += 1
                continue
            found.append((rel, kind, line, detail))
        if runs:
            notes.append("%s: %d runs of 32 bytes or more also in the stock firmware (the SDK and the "
                         "toolchain's runtime: docs/build.md, \"Release packages\")" % (rel, runs))
        if rel.endswith(".bin") and private:
            low = data.lower()
            for s in private:
                for form in (s.encode(), s.encode("utf-16-le")):
                    if form and form in low:
                        found.append((rel, "private", 0, "a string from materials/private-strings.txt"))
    return found, notes


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--twice", action="store_true")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "release"))
    ap.add_argument("--date", default=None)
    a = ap.parse_args(argv)
    if a.date is not None:
        try:
            if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", a.date):
                raise ValueError
            datetime.date.fromisoformat(a.date)
        except ValueError:
            ap.error("--date must be YYYY-MM-DD")
    try:
        out = validate_output(a.out)
    except (OSError, ValueError) as error:
        print("release: " + str(error), file=sys.stderr)
        return 1
    if not a.no_build and not build():
        return 2
    imgs = images()
    try:
        files = pack(out, imgs, a.date)
    except (OSError, ValueError) as error:
        print("release: could not prepare package: " + str(error), file=sys.stderr)
        return 1
    print("release: %d files in %s" % (len(files), os.path.relpath(out, ROOT)))
    for n, i in imgs.items():
        print("release:   %-15s %7d bytes  build %s  sha256 %s" % (
            n, os.path.getsize(os.path.join(ROOT, i["bin"])), i["build_id"][:8], i["sha256"][:16]))
    status = 0
    found, notes = scan(out, files)
    for n in notes:
        print("release: note: " + n)
    for rel, kind, line, detail in found:
        print("release: check_public: %s%s: %s: %s" % (rel, ":%d" % line if line else "", kind,
                                                       detail if kind != "private" else "(hidden)"))
    print("release: the publication check: %d findings" % len(found))
    if found:
        status = 1
    if a.twice:
        if not build(force=True):
            return 2
        again = images()
        diff = [n for n in imgs if (imgs[n]["sha256"], imgs[n]["dict_sha256"]) != (again[n]["sha256"], again[n]["dict_sha256"])]
        for n in diff:
            what = [w for w, k in (("the image", "sha256"), ("its log dictionary", "dict_sha256")) if imgs[n][k] != again[n][k]]
            print("release: not reproducible: %s: %s differ (build %s, then %s)" % (
                n, " and ".join(what), imgs[n]["build_id"][:8], again[n]["build_id"][:8]))
        print("release: built twice: %d of %d images the same bytes" % (len(imgs) - len(diff), len(imgs)))
        if diff:
            status = 1
    if not any(os.path.exists(os.path.join(ROOT, f)) for f in ("LICENSE", "LICENSE.md", "LICENSE.txt")):
        print("release: note: no LICENSE file (SPEC D-7: MIT; its copyright line is the owner's)")
    return status


if __name__ == "__main__":
    sys.exit(main())

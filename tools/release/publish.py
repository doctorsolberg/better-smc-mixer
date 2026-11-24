#!/usr/bin/env python3
"""Publication helpers for GitHub Actions (docs/build.md, "Publishing"). Standard library only.

  publish.py expected PACKAGE_DIR EXPECTED_JSON
      exit 1 unless every image in the package has the build ID and SHA-256 the hardware-verified
      candidate had (release/expected.json, written with `publish.py record`)
  publish.py record PACKAGE_DIR EXPECTED_JSON
      write EXPECTED_JSON from a package (after the candidate passed its hardware checks)
  publish.py zip PACKAGE_DIR OUT_ZIP
      a deterministic zip of the package under one top folder better-smc-mixer-<version>/
  publish.py site PACKAGE_DIR SITE_SRC OUT_DIR [OLD_ZIP ...]
      the GitHub Pages site: SITE_SRC (the home page) and the package at the root, and each older
      release's package (OLD_ZIP, from GitHub Releases) under /v<version>/
  publish.py notes PACKAGE_DIR NOTES_MD
      print the release notes: NOTES_MD with {version} and {images} filled in
"""
import json
import os
import shutil
import sys
import zipfile

EPOCH = (2026, 1, 1, 0, 0, 0)  # every zip entry gets this time, so the zip depends on content only


def manifest(pkg):
    with open(os.path.join(pkg, "manifest.json"), encoding="utf-8") as f:
        m = json.load(f)
    if m.get("product") != "better-smc-mixer":
        raise SystemExit("publish: %s is not a better-smc-mixer package" % pkg)
    return m


def images(m):
    return {n: {"build_id": i["build_id"], "sha256": i["sha256"], "size": i["size"]}
            for n, i in sorted(m["images"].items())}


def expected(pkg, path):
    m = manifest(pkg)
    with open(path, encoding="utf-8") as f:
        want = json.load(f)
    if want.get("version") != m["version"]:
        print("publish: release/expected.json is for %s, the package is %s" % (want.get("version"), m["version"]))
        return 1
    got = images(m)
    bad = [n for n in sorted(set(got) | set(want["images"])) if got.get(n) != want["images"].get(n)]
    for n in bad:
        print("publish: %s: built %s, verified on the mixer %s" % (n, got.get(n), want["images"].get(n)))
    if bad:
        return 1
    print("publish: %d images identical to the hardware-verified candidate %s" % (len(got), m["version"]))
    return 0


def record(pkg, path):
    m = manifest(pkg)
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"version": m["version"], "images": images(m)}, f, indent=1, sort_keys=True)
        f.write("\n")
    print("publish: wrote %s for %s" % (path, m["version"]))
    return 0


def walk(top):
    for root, dirs, files in os.walk(top):
        dirs.sort()
        for name in sorted(files):
            p = os.path.join(root, name)
            if os.path.islink(p):
                raise SystemExit("publish: symlink in the package: " + p)
            yield os.path.relpath(p, top)


def make_zip(pkg, out):
    m = manifest(pkg)
    top = "better-smc-mixer-%s" % m["version"]
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for rel in walk(pkg):
            info = zipfile.ZipInfo("%s/%s" % (top, rel.replace(os.sep, "/")), EPOCH)
            info.external_attr = 0o644 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(os.path.join(pkg, rel), "rb") as f:
                z.writestr(info, f.read())
    print("publish: %s" % out)
    return 0


def copy_tree(src, dst):
    for rel in walk(src):
        d = os.path.join(dst, rel)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copyfile(os.path.join(src, rel), d)


def site(pkg, site_src, out, olds):
    m = manifest(pkg)
    if os.path.exists(out):
        raise SystemExit("publish: %s exists" % out)
    copy_tree(pkg, out)
    copy_tree(site_src, out)
    open(os.path.join(out, ".nojekyll"), "w").close()  # serve files as they are
    kept = []
    for z in olds:
        with zipfile.ZipFile(z) as zf:
            names = zf.namelist()
            top = names[0].split("/")[0]
            if not top.startswith("better-smc-mixer-") or any(not n.startswith(top + "/") or ".." in n.split("/") for n in names):
                raise SystemExit("publish: unexpected layout in " + z)
            version = top[len("better-smc-mixer-"):]
            if version == m["version"]:
                continue
            dst = os.path.join(out, "v" + version)
            for n in names:
                if n.endswith("/"):
                    continue
                p = os.path.join(dst, n[len(top) + 1:])
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with zf.open(n) as fsrc, open(p, "wb") as fdst:
                    shutil.copyfileobj(fsrc, fdst)
            kept.append(version)
    print("publish: site for %s, older releases kept: %s" % (m["version"], ", ".join(sorted(kept)) or "none"))
    return 0


def notes(pkg, md):
    m = manifest(pkg)
    rows = "\n".join("| `%s` | %d | `%s` | `%s` |" % (n, i["size"], i["build_id"][:12], i["sha256"])
                     for n, i in images(m).items())
    table = "| Image | Bytes | Build ID | SHA-256 |\n|---|---|---|---|\n" + rows
    with open(md, encoding="utf-8") as f:
        print(f.read().replace("{version}", m["version"]).replace("{images}", table))
    return 0


def main(argv):
    if len(argv) >= 3 and argv[0] == "expected":
        return expected(argv[1], argv[2])
    if len(argv) >= 3 and argv[0] == "record":
        return record(argv[1], argv[2])
    if len(argv) >= 3 and argv[0] == "zip":
        return make_zip(argv[1], argv[2])
    if len(argv) >= 4 and argv[0] == "site":
        return site(argv[1], argv[2], argv[3], argv[4:])
    if len(argv) >= 3 and argv[0] == "notes":
        return notes(argv[1], argv[2])
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

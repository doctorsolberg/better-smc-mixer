"""Checks the links between the repository's Markdown files.

Every relative link must name a file or directory that exists. A link to a Markdown file may be
followed by section names in quotes, the way the documents point into each other:

    [core.md](core.md), "The CPU clock"
    [link.md](link.md), "Configuration", "Autosave"

Each name must be a heading of that file (the whole heading, the heading's start before ':', ' ('
or ' —', or the part after a ': '), or a bold paragraph label (**Name.**, **Name:**, **Name**), case
not counted. Links with a '#' anchor are checked against the headings the same way.

  check_docs.py [ROOT]      report; exit 1 on a broken link

Standard library only.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_public  # noqa: E402

LINK = re.compile(r"\[[^\]\n]*\]\(([^)\s]+)\)((?:,\s*\"[^\"\n]+\")*)")
QUOTED = re.compile(r"\"([^\"\n]+)\"")
FENCE = re.compile(r"^```.*?^```", re.MULTILINE | re.DOTALL)
CODE = re.compile(r"`[^`\n]*`")
BOLD = re.compile(r"\*\*([^*\n]+?)[.:]?\*\*")


def norm(s):
    return " ".join(s.replace("`", "").split()).lower()


def slug(s):
    """GitHub's anchor for a heading."""
    s = norm(s)
    s = re.sub(r"[^\w\- ]", "", s)
    return s.replace(" ", "-")


class Target:
    def __init__(self, path):
        with open(path, encoding="utf-8") as f:
            text = f.read()
        body = FENCE.sub("", text)
        self.heads = [re.sub(r"^#+\s*", "", line).strip() for line in body.splitlines()
                      if re.match(r"^#{1,6}\s", line)]
        self.labels = {norm(m.group(1)) for m in BOLD.finditer(body)}
        self.slugs = {slug(h) for h in self.heads}

    def has(self, name):
        n = norm(name)
        for h in map(norm, self.heads):
            if h == n or h.endswith(": " + n):
                return True
            if h.startswith(n) and h[len(n):len(n) + 2] in (": ", " (", " —", ", ") or h[len(n):] == ":":
                return True
        return n in self.labels or n.rstrip(".:") in self.labels


def prose(text):
    """Mask code examples without shifting link offsets or their original line numbers."""
    scan = FENCE.sub(lambda m: ''.join('\n' if c == '\n' else ' ' for c in m.group(0)), text)
    return CODE.sub(lambda m: ' ' * len(m.group(0)), scan)


def check(root):
    """[(file, line, message)]"""
    out = []
    cache = {}
    for rel in check_public.published(root):
        if not rel.endswith(".md"):
            continue
        with open(os.path.join(root, rel), encoding="utf-8") as f:
            text = f.read()
        scan = prose(text)
        for m in LINK.finditer(scan):
            target = m.group(1)
            if re.match(r"^[a-z][a-z0-9+.-]*:", target, re.IGNORECASE) or target.startswith("#"):
                continue
            path_part, _, anchor = target.partition("#")
            path_part = re.sub(r":\d+(?:-\d+)?$", "", path_part)  # file.c:42
            line = text.count("\n", 0, m.start()) + 1
            path = os.path.normpath(os.path.join(root, os.path.dirname(rel), path_part))
            if not os.path.exists(path):
                out.append((rel, line, "no such file: " + target))
                continue
            names = QUOTED.findall(m.group(2))
            if not (path.endswith(".md") and (names or anchor)):
                continue
            if path not in cache:
                cache[path] = Target(path)
            t = cache[path]
            if anchor and anchor.lower() not in t.slugs:
                out.append((rel, line, "no heading for #%s in %s" % (anchor, path_part)))
            for name in names:
                if not t.has(name):
                    out.append((rel, line, "no section \"%s\" in %s" % (name, path_part)))
    return out


def main(argv):
    root = os.path.abspath(argv[0] if argv else os.path.join(os.path.dirname(__file__), "..", ".."))
    found = check(root)
    for rel, line, msg in found:
        print("check_docs: %s:%d: %s" % (rel, line, msg))
    print("check_docs: %d broken" % len(found))
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

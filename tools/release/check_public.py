"""Checks that the files to be published hold nothing private or vendor-owned (SPEC C-7, AGENTS.md).

The published files are everything under the repository root that .gitignore does not exclude
(build/, deps/, logs/ and materials/ are excluded there). In each, it looks for:

  binary       a file that is not UTF-8 text, or holds NUL bytes (dumps, images, .DS_Store)
  large        a file over 512 KB
  stock        a run of 40 bytes or more from the stock firmware: in binary files, and in hex byte
               lists and strings in text; needs the local stock files in materials/ (the update file
               *.fwsc, dumps/stock*.bin and dumps/smc-mixer_*.bin), skipped without them
  private      a string from materials/private-strings.txt (local, never published: one string per
               line, '#' comments; the serial number, Bluetooth addresses, names), case-insensitive;
               a line '!text' allows that whole text (a public link that contains a private name)
  home         an absolute home path (/Users/x, /home/x, C:\\Users\\x)
  email        an e-mail address (noreply@ and example.* addresses allowed)
  mac          a Bluetooth or Ethernet address written aa:bb:cc:dd:ee:ff (all-zero and all-FF allowed)
  language     Cyrillic text: repository files are in English
  names        names of other products that stay out of the repository

third_party/ is checked for binaries, size and stock bytes only: its authors' notices are kept as
they are.

  check_public.py [ROOT]        report; exit 1 on any finding
  check_public.py --list [ROOT] print the files that would be published

Standard library only.
"""
import os
import re
import sys

LARGE = 512 * 1024
SELF = "tools/release/check_public.py"  # holds the patterns it looks for
STOCK_DUMPS = ("stock", "smc-mixer_")
WINDOW = 32
HEX_MIN = 40

# Other products' names that the owner keeps out of the repository (2026-11-12).
NAMES = re.compile(r"\b(?:EC4|UC4|Faderfox)\b", re.IGNORECASE)
CYRILLIC = re.compile("[\u0400-\u04ff]")
HOME = re.compile(r"/Users/(?!me\b|you\b|USER\b|<)[A-Za-z][\w.-]*|/home/(?!user\b|<)[a-z][\w.-]*|"
                  r"[A-Za-z]:\\\\?Users\\\\?(?!<)\w+")
EMAIL = re.compile(r"(?<![\w.+-])[\w.+-]{1,64}@[A-Za-z0-9-]{1,63}(?:\.[A-Za-z0-9-]{1,63}){0,8}\.[A-Za-z]{2,24}\b")
EMAIL_OK = re.compile(r"^(?:noreply@|.*@example\.(?:com|org|net)$)", re.IGNORECASE)
MAC = re.compile(r"(?<![0-9A-Fa-f:])[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}(?![0-9A-Fa-f:])")
MAC_OK = {"00:00:00:00:00:00", "ff:ff:ff:ff:ff:ff"}
# A list of bytes written in hex: "0x12, 0x34, ..." or "12 34 56 ..." (one separator kind per run).
HEX_LIST = re.compile(r"(?:0x[0-9A-Fa-f]{2}\s*,\s*){%d,}0x[0-9A-Fa-f]{2}|(?:\b[0-9A-Fa-f]{2} ){%d,}[0-9A-Fa-f]{2}\b"
                      % (HEX_MIN - 1, HEX_MIN - 1))
# Bytes written as one hex string, as the test vectors keep them.
HEX_STRING = re.compile(r"(?<![0-9A-Fa-f])(?:[0-9A-Fa-f]{2}){%d,}(?![0-9A-Fa-f])" % HEX_MIN)


def gitignore_rules(root):
    """The subset of .gitignore this repository uses: '/dir/' anchored, 'name' and 'dir/' anywhere."""
    rules = []
    path = os.path.join(root, ".gitignore")
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                anchored = line.startswith("/")
                is_dir = line.endswith("/")
                rules.append((line.strip("/"), anchored, is_dir))
    return rules


def ignored(rel, is_dir, rules):
    name = rel.rsplit("/", 1)[-1]
    for pattern, anchored, dir_only in rules:
        if dir_only and not is_dir:
            continue
        if anchored:
            if rel == pattern:
                return True
        elif name == pattern:
            return True
    return False


def published(root):
    rules = gitignore_rules(root)
    out = []
    for d, dirs, files in os.walk(root):
        reld = os.path.relpath(d, root).replace(os.sep, "/")
        reld = "" if reld == "." else reld + "/"
        dirs[:] = sorted(x for x in dirs if x != ".git" and not ignored(reld + x, True, rules))
        for f in sorted(files):
            if not ignored(reld + f, False, rules):
                out.append(reld + f)
    return out


def load_stock(root):
    """An index of the stock firmware's bytes from the local materials, or None when they are not
    here: every 32-byte window starting at a multiple of 8 in each dump. Any run of 40 bytes or more
    that the stock firmware holds contains one of them."""
    index = set()
    mat = os.path.join(root, "materials")
    for sub in ("dumps", ""):
        d = os.path.join(mat, sub)
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            # Only what the stock firmware is made of: its update file and the dumps read before our
            # firmware was installed. Later dumps hold our own images and configurations too.
            if not (f.endswith(".fwsc") or (f.endswith(".bin") and f.lower().startswith(STOCK_DUMPS))):
                continue
            with open(os.path.join(d, f), "rb") as fh:
                blob = fh.read()
            for off in range(0, len(blob) - WINDOW + 1, 8):
                chunk = blob[off:off + WINDOW]
                if not plain(chunk):
                    index.add(chunk)
    return index or None


def plain(chunk):
    """Too regular to mean anything: erased flash, zero fill, a short repeated pattern."""
    return len(set(chunk)) < 8


def stock_runs(data, stock):
    """Offsets where data holds a window of the stock firmware's index (one per run)."""
    hits = []
    last = -WINDOW
    for off in range(0, len(data) - WINDOW + 1):
        if off < last + WINDOW:
            continue
        chunk = data[off:off + WINDOW]
        if chunk in stock:
            hits.append(off)
            last = off
    return hits


def load_private(root):
    path = os.path.join(root, "materials", "private-strings.txt")
    if not os.path.exists(path):
        return None
    out, allowed = [], []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line.startswith("!") and len(line) > 1:
                allowed.append(line[1:].lower())
            elif line and not line.startswith("#"):
                out.append(line.lower())
    return Private(out, allowed)


class Private(list):
    """The private strings, with the texts allowed although they contain one."""

    def __init__(self, strings, allowed=()):
        super().__init__(strings)
        self.allowed = list(allowed)

    def mask(self, low):
        """low with every allowed text blanked out (same length, so line numbers hold)."""
        for a in self.allowed:
            low = low.replace(a, " " * len(a))
        return low


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def check_file(rel, data, stock, private):
    """[(kind, line or 0, detail)] for one file."""
    found = []
    third = rel.startswith("third_party/")
    if len(data) > LARGE:
        found.append(("large", 0, "%d bytes" % len(data)))
    try:
        text = data.decode("utf-8")
        binary = "\0" in text
    except UnicodeDecodeError:
        text, binary = None, True
    if binary:
        found.append(("binary", 0, "not plain UTF-8 text"))
        if stock is not None:
            for off in stock_runs(data, stock):
                found.append(("stock", 0, "stock firmware bytes at offset %d" % off))
        return found
    if stock is not None:
        for m in HEX_LIST.finditer(text):
            raw = bytes(int(h, 16) for h in re.findall(r"(?:0x)?([0-9A-Fa-f]{2})", m.group(0)))
            if stock_runs(raw, stock):
                found.append(("stock", line_of(text, m.start()), "a hex list of stock firmware bytes"))
        for m in HEX_STRING.finditer(text):
            if stock_runs(bytes.fromhex(m.group(0)), stock):
                found.append(("stock", line_of(text, m.start()), "a hex string of stock firmware bytes"))
    if third or rel == SELF:
        return found
    low = text.lower()
    if isinstance(private, Private):
        low = private.mask(low)
    for s in private or ():
        i = low.find(s)
        if i >= 0:
            found.append(("private", line_of(text, i), "a string from materials/private-strings.txt"))
    for m in HOME.finditer(text):
        found.append(("home", line_of(text, m.start()), "an absolute home path"))
    for kind, rx in (("language", CYRILLIC), ("names", NAMES)):
        for m in rx.finditer(text):
            found.append((kind, line_of(text, m.start()), m.group(0)))
    for m in EMAIL.finditer(text):
        if not EMAIL_OK.match(m.group(0)):
            found.append(("email", line_of(text, m.start()), "an e-mail address"))
    for m in MAC.finditer(text):
        if m.group(0).lower() not in MAC_OK:
            found.append(("mac", line_of(text, m.start()), "a hardware address"))
    return found


def check(root):
    """(findings, notes): findings are (path, kind, line, detail)."""
    stock = load_stock(root)
    private = load_private(root)
    notes = []
    if stock is None:
        notes.append("no stock firmware dumps in materials/: the stock-bytes check was skipped")
    if private is None:
        notes.append("no materials/private-strings.txt: the private-strings check was skipped")
    findings = []
    for rel in published(root):
        with open(os.path.join(root, rel), "rb") as fh:
            data = fh.read()
        for kind, line, detail in check_file(rel, data, stock, private):
            findings.append((rel, kind, line, detail))
    return findings, notes


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    root = os.path.abspath(args[0] if args else os.path.join(os.path.dirname(__file__), "..", ".."))
    if "--list" in argv:
        for rel in published(root):
            print(rel)
        return 0
    findings, notes = check(root)
    for n in notes:
        print("check_public: note: " + n)
    for rel, kind, line, detail in findings:
        # The detail of a private finding is never printed: the report may itself be shared.
        where = "%s:%d" % (rel, line) if line else rel
        print("check_public: %s: %s: %s" % (where, kind, detail if kind not in ("private",) else "(hidden)"))
    print("check_public: %d files, %d findings" % (len(published(root)), len(findings)))
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

"""Tests for rampath.py: synthetic listings in objdump's pi32v2 format, and the built images when
they are present (build/app, build/hwtest). Standard library only."""
import io
import os
import sys
import unittest
from contextlib import redirect_stderr, redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rampath as R  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def listing(*blocks):
    """blocks: (name, address, [instruction text, ...]); each instruction takes 4 bytes."""
    out = ["", "Disassembly of section .text:"]
    for name, addr, insns in blocks:
        out += ["", "", "%s:" % name]
        for i, text in enumerate(insns):
            if text.startswith("|"):  # a continuation line
                out.append("                               \t%s" % text[1:])
                continue
            out.append("%8x:    00 00 00 00       \t%s" % (addr + 4 * i, text))
    return "\n".join(out) + "\n"


def call(addr, name="f"):
    return "call 0 <%s : %x >" % (name, addr)


ISR = 0x17000
HELPER = 0x17100
FLASH_FN = 0x1E07000


def run(text, roots=("isr",), **kw):
    return R.walk(R.parse(text), list(roots), kw.pop("indirect", {}), **kw)


class Walk(unittest.TestCase):
    def test_ram_path_is_clean(self):
        res = run(listing(("isr", ISR, [call(HELPER, "helper"), "r0 = 1967872 <t : 1e0700 >", "rti"]),
                          ("helper", HELPER, ["r0 = 5", "rts"])))
        self.assertEqual(res.failures, [])
        self.assertEqual(sorted(res.reached), [ISR, HELPER])

    def test_flash_function(self):
        res = run(listing(("isr", ISR, [call(FLASH_FN, "slow"), "rti"]),
                          ("slow", FLASH_FN, ["rts"])))
        self.assertEqual(len(res.failures), 1)
        self.assertIn("slow", res.failures[0])
        self.assertIn("flash", res.failures[0])

    def test_far_target_is_masked(self):
        # objdump shows some branches from flash to RAM above 0x2000000
        res = run(listing(("isr", ISR, ["call 0 <flash_code_end+0x1 : %x >" % (HELPER + 0x2000000), "rti"]),
                          ("helper", HELPER, ["rts"])))
        self.assertEqual(res.failures, [])
        self.assertIn(HELPER, res.reached)

    def test_flash_constant(self):
        res = run(listing(("isr", ISR, ["r6 = 31508656 <tab : 1e0c8b0 >", "rti"])))
        self.assertEqual(len(res.failures), 1)
        self.assertIn("0x1e0c8b0", res.failures[0])

    def test_flash_constant_in_a_block(self):
        res = run(listing(("isr", ISR, ["if (r1 == 0) {", "|\tr0 = 31508656", "|} else { <tab : 1e0c8b0 >", "rti"])))
        self.assertEqual(len(res.failures), 1)

    def test_target_on_the_closing_line(self):
        res = run(listing(("isr", ISR, ["if (r0 >= 0x2) {", "|\tcall -0x10", "|} <helper : %x >" % HELPER, "rti"]),
                          ("helper", HELPER, ["rts"])))
        self.assertIn(HELPER, res.reached)

    def test_branch_inside_is_not_a_call(self):
        res = run(listing(("isr", ISR, ["if (r0 == 0) goto 4 <isr+0x8 : %x >" % (ISR + 8), "r0 = 1", "rti"]),
                          ("next", ISR + 0x100, ["rts"])))
        self.assertEqual(sorted(res.reached), [ISR])

    def test_fall_through(self):
        res = run(listing(("isr", ISR, ["r0 = 1"]), ("tail", ISR + 4, ["rti"])))
        self.assertEqual(sorted(res.reached), [ISR, ISR + 4])

    def test_indirect(self):
        text = listing(("isr", ISR, ["call r2", "rti"]), ("sink", HELPER, ["rts"]))
        res = run(text)
        self.assertEqual(len(res.failures), 1)
        self.assertIn("--indirect isr=", res.failures[0])
        res = run(text, indirect={"isr": ["sink"]})
        self.assertEqual(res.failures, [])
        self.assertIn(HELPER, res.reached)
        res = run(text, indirect={"isr": ["nothing"]})
        self.assertEqual(len(res.failures), 1)

    def test_rom(self):
        text = listing(("isr", ISR, [call(0x117F74, "memcpy"), call(0x117E58, "OS_ClrPending"), "rti"]))
        self.assertEqual(run(text).failures, [])  # trusted without --rom-ok
        res = run(text, rom_ok={"memcpy"})
        self.assertEqual(len(res.failures), 1)
        self.assertIn("OS_ClrPending", res.failures[0])
        self.assertEqual(run(text, rom_ok={"memcpy", "OS_ClrPending"}).failures, [])

    def test_forbid(self):
        res = run(listing(("isr", ISR, [call(HELPER, "os_sem_post"), "rti"]), ("os_sem_post", HELPER, ["rts"])),
                  forbid=["os_*"])
        self.assertEqual(len(res.failures), 1)
        self.assertIn("os_sem_post", res.failures[0])

    def test_explore_follows_flash(self):
        text = listing(("isr", ISR, [call(FLASH_FN, "slow"), "rti"]),
                       ("slow", FLASH_FN, [call(FLASH_FN + 0x100, "slower"), "rts"]),
                       ("slower", FLASH_FN + 0x100, ["rts"]))
        self.assertEqual(len(run(text).failures), 1)
        self.assertEqual(len(run(text, explore=True).failures), 2)

    def test_missing_root(self):
        res = run(listing(("isr", ISR, ["rti"])), roots=("nope",))
        self.assertIn("root nope", res.failures[0])


class Cli(unittest.TestCase):
    def main(self, text, *args):
        path = os.path.join(self.tmp, "app.lst")
        with open(path, "w") as f:
            f.write(text)
        buf = io.StringIO()
        with redirect_stdout(buf), redirect_stderr(io.StringIO()):
            code = R.main([path] + list(args))
        return code, buf.getvalue()

    def setUp(self):
        import tempfile
        self.tmp = tempfile.mkdtemp()

    def test_exit_codes(self):
        good = listing(("isr", ISR, ["rti"]))
        bad = listing(("isr", ISR, [call(FLASH_FN, "slow"), "rti"]), ("slow", FLASH_FN, ["rts"]))
        self.assertEqual(self.main(good, "--root", "isr")[0], 0)
        code, out = self.main(bad, "--root", "isr", "--quiet")
        self.assertEqual(code, 1)
        self.assertIn("FAIL", out)
        self.assertEqual(self.main(good, "--root", "isr", "--indirect", "nonsense")[0], 2)

    def test_parse_indirect(self):
        self.assertEqual(R.parse_indirect(["a=b,c", "a=d"]), {"a": ["b", "c", "d"]})
        with self.assertRaises(ValueError):
            R.parse_indirect(["a="])


class Built(unittest.TestCase):
    """The images tools/env/build_app.sh built, if any: the scan interrupt's path is clean."""

    def check(self, variant, sink):
        path = os.path.join(ROOT, "build", variant, "app.lst")
        if not os.path.exists(path):
            self.skipTest("no %s build" % variant)
        with open(path) as f:
            funcs = R.parse(f.read())
        res = R.walk(funcs, ["scan_isr"], {"send": [sink]},
                     rom_ok={"memcpy", "memset", "xTaskGetCurrentTaskHandle"},
                     forbid=["os_*", "local_irq_*"])
        if "root scan_isr" in " ".join(res.failures):
            self.skipTest("an older %s build" % variant)
        self.assertEqual(res.failures, [])

    def test_app(self):
        self.check("app", "sink")

    def test_hwtest(self):
        self.check("hwtest", "evq_sink")


if __name__ == "__main__":
    unittest.main()

"""Tests for compare.py on synthetic flash images. Run: python3 -I tools/jlrom/test_compare.py"""
import os
import random
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare  # noqa: E402

PLAIN_FROM = 0x6000
VM_END = 0x7000   # the test images' "stock VM": the sector at PLAIN_FROM


def images(base, chipkey=0x980F, seed=1):
    """A stock-view dump and the matching ROM read: sector 0 through the 256-byte keystream, SFC
    between 0x1000 and PLAIN_FROM."""
    rnd = random.Random(seed)
    dump = bytearray(rnd.randbytes(0x8000))
    rom = bytearray(dump)
    ks = bytearray(256)
    compare.enc_cipher(ks, 0, 256, 0xFFFF)
    rom[:0x1000] = bytes(b ^ ks[i % 256] for i, b in enumerate(dump[:0x1000]))
    rom[0x1000:PLAIN_FROM] = compare.sfc(dump, 0x1000, PLAIN_FROM, chipkey, base)
    return bytes(rom), bytes(dump)


class Cipher(unittest.TestCase):
    def test_enc_first_bytes(self):
        # key 0xFFFF: 0xFF, then (0xFFFF << 1) ^ 0x1021 = 0xEFDF -> 0xDF, then 0xDF9F -> 0x9F
        buf = bytearray(3)
        compare.enc_cipher(buf, 0, 3, 0xFFFF)
        self.assertEqual(bytes(buf), b"\xff\xdf\x9f")

    def test_sfc_is_symmetric(self):
        data = bytes(range(256)) * 16
        once = compare.sfc(data, 0, len(data), 0x980F, 0x1000)
        self.assertNotEqual(bytes(once), data)
        self.assertEqual(bytes(compare.sfc(once, 0, len(once), 0x980F, 0x1000)), data)

    def test_block_key_depends_on_address(self):
        zeros = bytes(64)
        out = compare.sfc(zeros, 0, 64, 0x980F, 0)
        self.assertNotEqual(out[:32], out[32:])


class Compare(unittest.TestCase):
    def test_finds_the_base(self):
        for base in compare.BASES:
            rom, dump = images(base)
            ok, lines = compare.compare(rom, dump, 0x980F, PLAIN_FROM)
            self.assertTrue(ok, lines)
            self.assertIn(f"  -> base {base:#x} explains the whole SFC area", lines)

    def test_wrong_chipkey_fails(self):
        rom, dump = images(0x1000)
        ok, lines = compare.compare(rom, dump, 0x9C0F, PLAIN_FROM)
        self.assertFalse(ok)

    def test_sector0_difference_fails(self):
        rom, dump = images(0x1000)
        rom = bytearray(rom)
        rom[0x60] ^= 1
        ok, lines = compare.compare(bytes(rom), dump, 0x980F, PLAIN_FROM)
        self.assertFalse(ok)
        self.assertIn("sector 0 through the 256-byte keystream: 1 of 4096 bytes differ", lines)

    def test_plain_area_difference_is_located(self):
        rom, dump = images(0x1000)
        rom = bytearray(rom)
        rom[0x7123] ^= 1
        ok, lines = compare.compare(bytes(rom), dump, 0x980F, PLAIN_FROM, VM_END)
        self.assertFalse(ok)
        self.assertIn("  sector 0x07000: 1 bytes differ", lines)

    def test_vm_difference_is_marked_and_passes(self):
        """The stock firmware rewrites its VM: a difference there does not fail the read."""
        rom, dump = images(0x1000)
        rom = bytearray(rom)
        rom[0x6010] ^= 1
        rom[0x6FFF] ^= 1
        ok, lines = compare.compare(bytes(rom), dump, 0x980F, PLAIN_FROM, VM_END)
        self.assertTrue(ok, lines)
        self.assertIn("  sector 0x06000: 2 bytes differ (the stock firmware's VM)", lines)
        self.assertTrue(any("only the stock firmware's VM differs" in x for x in lines), lines)
        ok, lines = compare.compare(bytes(rom), dump, 0x980F, PLAIN_FROM, VM_END, strict=True)
        self.assertFalse(ok)

    def test_vm_and_another_sector_fail(self):
        rom, dump = images(0x1000)
        rom = bytearray(rom)
        rom[0x6010] ^= 1
        rom[0x7FFF] ^= 1
        ok, lines = compare.compare(bytes(rom), dump, 0x980F, PLAIN_FROM, VM_END)
        self.assertFalse(ok)
        self.assertTrue(any("2 sectors differ, 1 of them outside" in x for x in lines), lines)
        self.assertFalse(any("only the stock firmware's VM differs" in x for x in lines), lines)

    def test_default_vm_is_the_mixer_s(self):
        self.assertEqual(compare.VM_END, 0x6E000)
        a = bytearray(0x80000)
        b = bytearray(a)
        b[0x24000] = 1          # the VM's first sector
        b[0x6DFFF] = 1          # its last byte
        self.assertTrue(compare.compare_raw(bytes(a), bytes(b), (0x24000, compare.VM_END))[0])
        b[0x6E000] = 1          # the stock update loader
        self.assertFalse(compare.compare_raw(bytes(a), bytes(b), (0x24000, compare.VM_END))[0])


class Raw(unittest.TestCase):
    """Two ROM reads against each other."""

    VM = (PLAIN_FROM, VM_END)

    def test_equal(self):
        rom, _ = images(0x1000)
        ok, lines = compare.compare_raw(rom, rom, self.VM)
        self.assertTrue(ok)
        self.assertEqual(lines, ["raw reads 0x00000..0x08000: equal"])

    def test_vm_only(self):
        rom, _ = images(0x1000)
        other = bytearray(rom)
        other[0x6800] ^= 0xFF
        ok, lines = compare.compare_raw(rom, bytes(other), self.VM)
        self.assertTrue(ok, lines)
        self.assertFalse(compare.compare_raw(rom, bytes(other), self.VM, strict=True)[0])

    def test_sector0_or_code_fails(self):
        rom, _ = images(0x1000)
        for at in (0x60, 0x1120, 0x7000):
            other = bytearray(rom)
            other[at] ^= 1
            ok, lines = compare.compare_raw(rom, bytes(other), self.VM)
            self.assertFalse(ok, hex(at))
            self.assertIn(f"  sector {at & ~0xFFF:#07x}: 1 bytes differ", lines)

    def test_sizes_differ(self):
        rom, _ = images(0x1000)
        ok, lines = compare.compare_raw(rom, rom[:0x4000], self.VM)
        self.assertFalse(ok)
        self.assertIn("sizes differ: 0x8000 and 0x4000; comparing 0x4000", lines)

    def test_command_line(self):
        import subprocess
        import tempfile
        rom, dump = images(0x1000)
        other = bytearray(rom)
        other[0x6800] ^= 0xFF
        with tempfile.TemporaryDirectory() as d:
            paths = {}
            for name, data in (("a", rom), ("b", bytes(other)), ("dump", dump)):
                paths[name] = os.path.join(d, name)
                with open(paths[name], "wb") as f:
                    f.write(data)
            tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), "compare.py")
            common = ["--plain-from", hex(PLAIN_FROM), "--vm-end", hex(VM_END)]

            def run(*args):
                r = subprocess.run([sys.executable, "-I", tool, *args, *common], capture_output=True, text=True)
                return r.returncode, r.stdout
            self.assertEqual(run("--raw", paths["a"], paths["b"])[0], 0)
            self.assertEqual(run("--raw", "--strict", paths["a"], paths["b"])[0], 1)
            code, out = run(paths["b"], paths["dump"])
            self.assertEqual(code, 0, out)
            self.assertIn("result: ok", out)
            self.assertEqual(run(paths["b"], paths["dump"], "--strict")[0], 1)

    def test_report_holds_no_contents(self):
        rom, dump = images(0x1000)
        _, lines = compare.compare(rom, dump, 0x980F, PLAIN_FROM)
        text = "\n".join(lines)
        self.assertNotIn(dump[0x7000:0x7008].hex(), text)


if __name__ == "__main__":
    unittest.main()

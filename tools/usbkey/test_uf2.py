"""Tests for uf2.py. Run: python3 -I tools/usbkey/test_uf2.py"""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uf2  # noqa: E402


class Crc(unittest.TestCase):
    def test_check_value(self):
        # CRC-32/MPEG-2 has the same parameters as the boot ROM's check; its check value
        self.assertEqual(uf2.crc32_boot2(b"123456789"), 0x0376E6E7)

    def test_seal(self):
        image = bytes(range(256)) + b"rest"
        sealed = uf2.seal_boot2(image)
        self.assertEqual(sealed[:252], image[:252])
        self.assertEqual(struct.unpack("<I", sealed[252:256])[0], uf2.crc32_boot2(image[:252]))
        self.assertEqual(sealed[256:], b"rest")


class Blocks(unittest.TestCase):
    def test_layout(self):
        image = bytes(600)
        out = uf2.uf2(image)
        self.assertEqual(len(out), 3 * 512)
        for n in range(3):
            b = out[n * 512:(n + 1) * 512]
            m0, m1, flags, addr, size, no, count, fam = struct.unpack("<IIIIIIII", b[:32])
            self.assertEqual((m0, m1, flags, fam), (uf2.MAGIC0, uf2.MAGIC1, uf2.FLAG_FAMILY_ID, uf2.FAMILY_RP2040))
            self.assertEqual((addr, size, no, count), (0x10000000 + 256 * n, 256, n, 3))
            self.assertEqual(struct.unpack("<I", b[508:512])[0], uf2.MAGIC_END)


if __name__ == "__main__":
    unittest.main()

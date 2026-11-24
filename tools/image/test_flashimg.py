"""Tests for flashimg.py. Run: python3 -I tools/image/test_flashimg.py

Synthetic tests always run. The stock rebuild runs only where the vendor files are present
(materials/, never in the repository): it is skipped elsewhere.
"""
import hashlib
import os
import random
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import flashimg as F  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
DUMP = os.path.join(ROOT, "materials", "dumps", "smc-mixer_012_dump1.bin")
FWSC = os.path.join(ROOT, "materials", "SMC-Mixer_013.fwsc")


def lfsr(key):
    return ((key << 1) ^ 0x1021) & 0xFFFF if key & 0x8000 else (key << 1) & 0xFFFF


class Ciphers(unittest.TestCase):
    def test_crc16_check_value(self):
        self.assertEqual(F.crc16(b"123456789"), 0x31C3)  # CRC-16/XMODEM
        self.assertEqual(F.crc16(b""), 0)

    def test_keystream(self):
        # 0xFFFF -> 0xFF; (0xFFFF << 1) ^ 0x1021 = 0xEFDF -> 0xDF; then 0xDF9F -> 0x9F
        self.assertEqual(bytes(F.keystream(0xFFFF, 3)), b"\xff\xdf\x9f")
        self.assertEqual(F.keystream(0x1234, 10, 4), F.keystream(0x1234, 14)[4:])

    def test_enc_is_symmetric(self):
        data = bytes(range(200))
        self.assertEqual(F.enc(F.enc(data, 0xFFFF), 0xFFFF), data)

    def test_sfc_block_keys(self):
        # The block at flash 0x1000 is keyed with the chip key itself, the next one with ^ 8.
        z = F.sfc(bytes(64), 0x1000, 0x980F)
        self.assertEqual(z[:32], bytes(F.keystream(0x980F, 32)))
        self.assertEqual(z[32:], bytes(F.keystream(0x980F ^ 8, 32)))
        self.assertEqual(F.sfc_key(0x2000), 0x980F ^ 0x400)

    def test_sfc_any_alignment(self):
        data = random.Random(1).randbytes(300)
        whole = F.sfc(data, 0x1000)
        self.assertEqual(F.sfc(data[45:], 0x1000 + 45), whole[45:])
        self.assertEqual(F.sfc(whole, 0x1000), data)

    def test_sfc_slots_on_a_256k_grid_share_the_cipher(self):
        # The key uses address bits 2..17: a slot 0x40000 further encrypts the same.
        data = bytes(range(256))
        self.assertEqual(F.sfc(data, 0x1000), F.sfc(data, 0x41000))
        self.assertNotEqual(F.sfc(data, 0x1000), F.sfc(data, 0x25000))


def sector0(boot=0x1000):
    """A synthetic physical sector 0: a header entry, the four stock list entries, 0xFF."""
    head = F.Entry("hdr", 0x12345678, 0x7F000, 3, 0x10, 0xA4, 0x220)
    lst = [F.Entry("uboot.boot", 0xA0, 0x100, 0x40, 0, 0, 0x1111),
           F.Entry("isd_config.ini", 0x1A0, 0x63, 2, 0xFF, 0, 0x2222),
           F.Entry("app_dir_head", boot, 0xFFFFFFFF, 0x81, 0xFF, 0),
           F.Entry("key_mac", 0x7F000, 0x1000, 0x12, 1, 0xFFFF)]
    out = b"".join(F.enc(e.pack(), 0xFFFF) for e in [head] + lst)
    return out + b"\xff" * (F.SECTOR - len(out))


class Sector0(unittest.TestCase):
    def test_entries(self):
        names = [e.name for e in F.sector0_entries(sector0())[1:]]
        self.assertEqual(names, ["uboot.boot", "isd_config.ini", "app_dir_head", "key_mac"])

    def test_view_is_symmetric_and_restarts_every_256_bytes(self):
        s = sector0()
        v = F.sector0_view(s)
        self.assertEqual(F.sector0_view(v), s)
        ks = bytes(F.keystream(0xFFFF, 256))
        self.assertEqual(F.sector0_view(bytes(512)), ks + ks)
        # The first 32 bytes of the view are the header in clear.
        self.assertIsNotNone(F.Entry.parse(v[:32]))

    def test_boot_pointer_changes_one_entry(self):
        s = sector0()
        t = F.set_boot_pointer(s, 0x41000)
        diff = [i for i in range(len(s)) if s[i] != t[i]]
        self.assertTrue(diff and min(diff) >= 0x60 and max(diff) < 0x80, diff)
        e = F.sector0_entries(t)[3]
        self.assertEqual((e.name, e.offset), ("app_dir_head", 0x41000))
        self.assertEqual(F.set_boot_pointer(t, 0x1000), s)

    def test_bad_entry_is_refused(self):
        s = bytearray(sector0())
        s[0x45] ^= 1
        with self.assertRaises(ValueError):
            F.sector0_entries(bytes(s))


class Entries(unittest.TestCase):
    def test_pack_parse(self):
        e = F.Entry("PRCT", 0, 0x24000, 0x92, 0x82, 0, 0xFFFF, b"CODE")
        b = e.pack()
        self.assertEqual(len(b), 32)
        self.assertEqual(b[16:32], b"PRCT\0" + b"\xff" * 7 + b"CODE")
        self.assertEqual(F.Entry.parse(b), e)
        self.assertEqual(struct.unpack_from("<H", b)[0], F.crc16(b[2:]))

    def test_bad_crc(self):
        b = bytearray(F.Entry("app.bin", 0x120, 10, 0x82).pack())
        b[20] ^= 1
        self.assertIsNone(F.Entry.parse(bytes(b)))

    def test_name_too_long(self):
        with self.assertRaises(ValueError):
            F.Entry("a" * 12, 0, 0, 0, tag=b"CODE").pack()


def our_area(app_len=1000, cfg_len=0x360, seed=2):
    rnd = random.Random(seed)
    return F.Area(F.ENTRY, [("app.bin", rnd.randbytes(app_len)), ("cfg_tool.bin", rnd.randbytes(cfg_len))],
                  F.stock_regions())


class Area(unittest.TestCase):
    def test_layout(self):
        area = our_area()
        plain = F.build_area(area)
        ents = F.read_entries(plain)
        self.assertEqual([e.name for e in ents], ["app_area_head", "app.bin", "cfg_tool.bin", "VM",
                                                  "PRCT", "BTIF", "EXIF", "USRFLASH"])
        head = ents[0]
        self.assertEqual((head.offset, head.size, head.attr), (F.ENTRY, 0x120 + 1000 + 0x360, 0x83))
        self.assertEqual(head.data_crc, F.crc16(plain[0x20:head.size]))
        self.assertEqual(ents[1].offset, 0x120)  # the entry point 0x1E00120 is app.bin's start
        self.assertEqual(ents[2].offset, 0x120 + 1000)
        self.assertEqual(plain[0x100:0x120], b"\xff" * 32)  # the ninth slot stays empty
        self.assertEqual([e.index for e in ents], [0] * 7 + [1])
        self.assertEqual(len(plain), head.size)

    def test_round_trip(self):
        area = our_area()
        area.tail = b"tail"
        plain = F.build_area(area)
        back = F.parse_area(plain)
        self.assertEqual(back.files, area.files)
        self.assertEqual(back.regions, area.regions)
        self.assertEqual(back.entry, area.entry)
        self.assertEqual(plain[back.size:], b"tail")

    def test_corruption_is_found(self):
        plain = bytearray(F.build_area(our_area()))
        plain[0x200] ^= 1
        with self.assertRaises(ValueError):
            F.parse_area(bytes(plain))

    def test_area_must_end_below_the_vm(self):
        area = our_area()
        area.regions = F.stock_regions(0x1800, 0x1000)  # the area ends at 0x1868
        with self.assertRaises(ValueError):
            F.build_area(area)

    def test_directory_capacity(self):
        area = our_area()
        area.files.append(("extra.bin", b"x"))
        F.build_area(area)  # nine entries fill the directory
        area.files.append(("more.bin", b"y"))
        with self.assertRaises(ValueError):
            F.build_area(area)

    def test_physical(self):
        plain = F.build_area(our_area())
        phys = F.physical(plain, 0x1000, F.CHIPKEY, 0x24000)
        self.assertEqual(len(phys), 0x23000)
        self.assertEqual(phys[len(plain):], b"\xff" * (0x23000 - len(plain)))
        self.assertEqual(F.sfc(phys[:len(plain)], 0x1000), plain)

    def test_stock_tail(self):
        app = b"app"
        e = F.Entry("config.dat", 0x20, 4, 0x82, data_crc=0x54F2)
        tail = F.stock_tail(b"cfg!", e, app)
        self.assertEqual(tail[32:36], b"cfg!")
        self.assertEqual(tail[36:], hashlib.md5(app).hexdigest().encode())
        self.assertEqual(F.parse_stock_tail(tail, 0), (e, b"cfg!", tail[36:]))


class Containers(unittest.TestCase):
    def test_fwsc_round_trip(self):
        ufw = random.Random(3).randbytes(4000)
        raw = F.fwsc_wrap("SMC-Mixer", "013", ufw)
        self.assertEqual(len(raw), len(ufw) + 20)
        self.assertEqual(F.fwsc_unwrap(raw), ("SMC-Mixer", "013", ufw))

    def test_fwsc_pad_equal_to_the_last_digit(self):
        # Metadata byte 13 of "SMC-Mixer_013" is ord('3') + 13 = 0x40, the same as the pad.
        raw = F.fwsc_wrap("SMC-Mixer", "013", bytes(2000), pad=0x40)
        self.assertEqual(raw[12 * 0x30 + 0x2F], 0x40)
        self.assertEqual(F.fwsc_unwrap(raw)[:2], ("SMC-Mixer", "013"))

    def test_fwsc_36_blocks(self):
        raw = F.fwsc_wrap("SMC-PAD", "005", bytes(range(256)) * 10, n=36, pad=0x7D)
        self.assertEqual(F.fwsc_unwrap(raw), ("SMC-PAD", "005", bytes(range(256)) * 10))

    def test_not_fwsc(self):
        with self.assertRaises(ValueError):
            F.fwsc_unwrap(bytes(4000))

    def test_ufw(self):
        data = b"payload!" * 8
        hdr = bytearray(0x40)
        struct.pack_into("<I", hdr, 4, 0x40 + 2 * 0xA0 + 2 * len(data))
        hdr[16:22] = b"AC636N"
        struct.pack_into("<H", hdr, 0, F.crc16(hdr[2:]))
        recs = b""
        for i, (name, addr) in enumerate((("flash.bin", 0), ("USRFLASH", 0x7D000))):
            r = bytearray(0xA0)
            off = 0x40 + 2 * 0xA0 + i * len(data)
            struct.pack_into("<III", r, 4, F.crc16(data), off, len(data))
            struct.pack_into("<I", r, 0x1C, addr)
            r[0x40:0x40 + len(name)] = name.encode()
            recs += F.enc(bytes(r), 0xFFFF)
        ufw = F.enc(bytes(hdr), 0xFFFF) + recs + data + data
        self.assertEqual(F.ufw_header(ufw), ("AC636N", len(ufw)))
        files = F.ufw_files(ufw)
        self.assertEqual(files["flash.bin"], (0x180, len(data), True, None))
        self.assertEqual(files["USRFLASH"], (0x180 + len(data), len(data), True, 0x7D000))


class Cli(unittest.TestCase):
    def test_build(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            app = os.path.join(d, "app.bin")
            with open(app, "wb") as f:
                f.write(b"\x01" * 100)
            out, phys = os.path.join(d, "area.bin"), os.path.join(d, "phys.bin")
            import io
            from contextlib import redirect_stdout
            with redirect_stdout(io.StringIO()):
                self.assertEqual(F.main(["build", "--app", app, "-o", out, "--physical", phys]), 0)
            with open(out, "rb") as f:
                plain = f.read()
            self.assertEqual(F.parse_area(plain).files, [("app.bin", b"\x01" * 100)])
            with open(phys, "rb") as f:
                self.assertEqual(F.sfc(f.read()[:len(plain)], 0x1000), plain)


@unittest.skipUnless(os.path.exists(DUMP) and os.path.exists(FWSC), "vendor files not present")
class Stock(unittest.TestCase):
    """The stock areas of V012 (dump) and V013 (vendor update) rebuild byte for byte."""

    def test_check(self):
        with open(DUMP, "rb") as f:
            dump = f.read()
        with open(FWSC, "rb") as f:
            fwsc = f.read()
        ok, lines = F.check(dump, fwsc)
        self.assertTrue(ok, "\n".join(lines))


if __name__ == "__main__":
    unittest.main()

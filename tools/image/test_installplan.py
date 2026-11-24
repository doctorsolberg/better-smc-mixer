"""Tests for installplan.py. Run: python3 -I tools/image/test_installplan.py

On a synthetic stock flash (sector 0, a stock area, the stock VM's data, an update loader, BTIF with
a MAC address item, EXIF, USRFLASH), stored as the chip stores it and as the stock firmware reads
it. The run on the V012 dump happens only where it is present (materials/, never in the
repository).
"""
import contextlib
import io
import json
import os
import random
import re
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import flashimg as F  # noqa: E402
import installplan as IP  # noqa: E402
import test_flashimg as TF  # noqa: E402

DUMP = TF.DUMP


def btif_sector(r, mac="valid"):
    """BTIF: the MAC address item at 0 (valid, empty or something else), data of its own further on."""
    b = bytearray(b"\xff" * F.SECTOR)
    addr = r.randbytes(6)
    if mac == "valid":
        b[0:10] = struct.pack("<I", 6 << 20 | 102 << 8 | F.crc16(addr) & 0xFF) + addr
    elif mac == "other":
        b[0:10] = struct.pack("<I", 8 << 20 | 7 << 8) + addr
    b[0x400:0x500] = r.randbytes(0x100)
    return bytes(b)


def stock_flash(seed=3, mac="valid"):
    """A stock flash as stored."""
    r = random.Random(seed)
    img = bytearray(b"\xff" * IP.FLASH_SIZE)
    img[:F.SECTOR] = TF.sector0()
    area = F.Area(F.ENTRY, [("app.bin", r.randbytes(5000)), ("cfg_tool.bin", r.randbytes(0x360))],
                  F.stock_regions())
    img[F.AREA_AT:0x24000] = F.physical(F.build_area(area), F.AREA_AT, F.CHIPKEY, 0x24000)
    img[0x24000:0x24100] = r.randbytes(0x100)        # the stock VM's data
    img[0x6E000:0x71000] = r.randbytes(0x3000)       # the update loader
    img[0x7B000:0x7C000] = btif_sector(r, mac)
    img[0x7C000:0x7C100] = r.randbytes(0x100)        # EXIF
    img[0x7D000:0x7D200] = r.randbytes(0x200)        # USRFLASH
    return bytes(img)


def stock_read(img):
    """What the stock firmware's read returns of it."""
    return F.sector0_view(img[:F.SECTOR]) + F.sfc(img[F.AREA_AT:0x24000], F.AREA_AT) + img[0x24000:]


APP = random.Random(4).randbytes(9000)


def fails(p):
    return [i["text"] for i in p.report.items if i["status"] == "fail"]


def texts(p, section, status=None):
    return [i["text"] for i in p.report.items if i["section"] == section and (status is None or i["status"] == status)]


class Plan(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.img = stock_flash()
        cls.p = IP.plan(cls.img, APP)

    def test_it_holds(self):
        p = self.p
        self.assertEqual(fails(p), [])
        self.assertEqual(p.view, "raw")
        self.assertEqual(p.before, self.img)
        self.assertEqual([(w.what, w.offset, w.erase) for w in p.writes],
                         [("our area", IP.SLOT, tuple(range(IP.SLOT, IP.SLOT + 0x3000, F.SECTOR))),
                          ("the boot pointer", 0, (0,))])
        self.assertEqual(len(p.steps), 4)
        self.assertEqual(len(p.undo), 3)

    def test_the_flash_after_starts_our_app(self):
        for name, base in IP.KEY_MODELS:
            c = IP.boot_chain(self.p.after, base)
            self.assertEqual((c.pointer, c.entry_flash, c.entry_file, c.entry_at), (IP.SLOT, IP.SLOT + 0x120, "app.bin", 0))
            self.assertEqual(c.area.file("app.bin"), APP)
            vm = IP.region(c.area.regions, "VM")
            self.assertEqual((vm.offset, vm.offset + vm.size), (IP.SLOT + 0x3000, IP.VM_END))
            self.assertEqual((IP.region(c.area.regions, "PRCT").offset, IP.region(c.area.regions, "PRCT").size),
                             (0, vm.offset))
        self.assertEqual(len(texts(self.p, "after", "pass")), 4)

    def test_only_the_boot_pointer_and_our_sectors_change(self):
        a, b = self.img, self.p.after
        diff = [i for i in range(len(a)) if a[i] != b[i]]
        self.assertGreaterEqual(diff[0], 0x60)
        self.assertLess(max(i for i in diff if i < F.SECTOR), 0x80)
        self.assertTrue(all(IP.SLOT <= i < IP.SLOT + 0x3000 for i in diff if i >= F.SECTOR))
        self.assertEqual(F.sector0_entries(b[:F.SECTOR])[3].offset, IP.SLOT)
        self.assertEqual(IP.boot_chain(F.set_boot_pointer(b[:F.SECTOR], F.AREA_AT) + b[F.SECTOR:]).area.file("app.bin"),
                         IP.boot_chain(a).area.file("app.bin"))  # the stock area still there for the rollback

    def test_the_rollback_gives_the_flash_before(self):
        self.assertEqual(IP.apply(self.p.after, self.p.rollback), self.img)
        self.assertEqual(texts(self.p, "rollback", "pass"),
                         ["applied to the flash after, the rollback gives the flash before byte for byte"])

    def test_a_stock_read_gives_the_same_plan(self):
        p = IP.plan(stock_read(self.img), APP)
        self.assertEqual(fails(p), [])
        self.assertEqual(p.view, "stock")
        self.assertEqual(p.before, self.img)
        self.assertEqual(p.after, self.p.after)
        self.assertEqual(p.steps, self.p.steps)
        self.assertIn("this flash is a model until a raw ROM read confirms it (recovery.md, Rehearsal 1)",
                      texts(p, "open"))

    def test_the_boot_pointer_bits(self):
        old, new = self.img[0x60:0x80], self.p.after[0x60:0x80]
        n = IP.needs_erase(old, new)
        self.assertEqual(n, sum(bin(b & ~a & 0xFF).count("1") for a, b in zip(old, new)))
        info = [t for t in texts(self.p, "writes", "info") if t.startswith("the boot pointer entry")]
        self.assertEqual(len(info), 1)
        self.assertIn(f"{n} of its bits" if n else "without erasing", info[0])
        self.assertTrue(any(t.startswith("the stock write (0x22) stores sector 0 as sent")
                            for t in texts(self.p, "writes", "info")))

    def test_the_first_run(self):
        self.assertEqual(texts(self.p, "first run", "pass"),
                         ["BTIF item 102 (CFG_BT_MAC_ADDR) at 0x7b000: valid: our firmware reads the stock MAC "
                          "address and writes nothing to BTIF"])
        self.assertIn("BTIF item 110 (CFG_BT_FRE_OFFSET) at 0x7b00a: empty; our firmware only reads it",
                      texts(self.p, "first run", "info"))
        for mac, words in (("empty", "writes a random MAC address into it"), ("other", "refuses to write there")):
            p = IP.plan(stock_flash(mac=mac), APP)
            self.assertEqual(fails(p), [])
            self.assertTrue(any(words in t for t in texts(p, "first run", "info")), mac)

    def test_a_cfg_tool_and_a_tail(self):
        p = IP.plan(self.img, APP, cfg=b"\1" * 0x360, tail=b"\2" * 0x1C0)
        self.assertEqual(fails(p), [])
        c = IP.boot_chain(p.after)
        self.assertEqual([n for n, _ in c.area.files], ["app.bin", "cfg_tool.bin"])
        self.assertFalse(any("cfg_tool.bin" in t for t in texts(p, "open")))


class Refusals(unittest.TestCase):
    def test_not_the_stock_flash(self):
        p = IP.plan(IP.plan(stock_flash(), APP).after, APP)
        self.assertEqual(len(fails(p)), 1)
        self.assertIn("not the stock flash (already installed?)", fails(p)[0])
        self.assertTrue(fails(IP.plan(stock_flash()[:0x40000], APP))[0].startswith("the image: 0x40000 bytes"))
        junk = bytes(F.SECTOR) + stock_flash()[F.SECTOR:]
        self.assertIn("sector 0 parses neither", fails(IP.plan(junk, APP))[0])

    def test_our_sectors_in_use(self):
        img = bytearray(stock_flash())
        img[0x45000:0x45010] = bytes(16)
        self.assertEqual(fails(IP.plan(bytes(img), APP)), ["our sectors are in use: 0x45000..0x45fff"])

    def test_an_earlier_installation(self):
        # our firmware pointed sector 0 back at the stock area (the update path): the area stays,
        # its VM and the configuration's slots hold data
        img = bytearray(IP.plan(stock_flash(), random.Random(9).randbytes(20000)).after)
        img[:F.SECTOR] = F.set_boot_pointer(bytes(img[:F.SECTOR]), F.AREA_AT)
        img[0x6D000:0x6D040] = bytes(0x40)        # our VM's data
        img[0x25000:0x25020] = bytes(range(32))   # a saved configuration
        img = bytes(img)
        p = IP.plan(img, APP)
        self.assertEqual(fails(p), [])
        self.assertEqual(p.found, "earlier")
        self.assertTrue(any("hold an earlier installation of ours" in t for t in texts(p, "ours", "info")))
        self.assertEqual([(w.what, w.erase) for w in p.writes[:1]], [("our earlier sectors", (0x44000, 0x45000, 0x6D000))])
        self.assertEqual(IP.boot_chain(p.after).area.file("app.bin"), APP)
        self.assertEqual(p.after[0x25000:0x25020], bytes(range(32)))  # kept
        self.assertEqual(p.after[IP.SLOT + 0x3000:IP.VM_END], b"\xff" * (IP.VM_END - IP.SLOT - 0x3000))
        self.assertEqual(texts(p, "rollback", "pass"),
                         ["applied to the flash after, the rollback gives the flash before with our sectors erased byte for byte"])
        self.assertTrue(any(t.startswith("the configuration's two slots and the panel's state 0x25000..0x36fff "
                                         "(firmware/store/cfgstore.h, statestore.h): kept") for t in texts(p, "first run")))
        self.assertIn("(an earlier installation of ours).", p.steps[0])
        self.assertTrue(p.steps[1].startswith("2. Our area: erase the earlier installation's 3 sectors after it"))
        # read as the stock firmware reads it: the same
        self.assertEqual(IP.plan(stock_read(img), APP).after, p.after)

    def test_a_run_that_stopped(self):
        fresh = IP.plan(stock_flash(), APP)
        area = fresh.writes[0]
        for upto in (0x100, 0x1100, 0x2200, len(area.data)):
            img = bytearray(stock_flash())
            img[IP.SLOT + 0x1000:IP.SLOT + max(0x1000, upto)] = area.data[0x1000:max(0x1000, upto)]
            if upto < 0x1000:  # the directory's sector is written last: a page of the area's last
                img[IP.SLOT + 0x2000:IP.SLOT + 0x2000 + upto] = area.data[0x2000:0x2000 + upto]
            img = bytes(img)
            p = IP.plan(img, APP)
            self.assertEqual(fails(p), [], hex(upto))
            self.assertEqual(p.found, "stopped")
            self.assertEqual(p.after, fresh.after)
            self.assertIn("a run of this installation that stopped", texts(p, "ours", "pass")[-1])
        # once the directory's first page is written, it reads as an installation of ours: the same writes
        d = bytearray(img)
        d[IP.SLOT:IP.SLOT + 0x100] = area.data[:0x100]
        p = IP.plan(bytes(d), APP)
        self.assertEqual((fails(p), p.found, p.after), ([], "earlier", fresh.after))
        # the configuration's slots in use too (an update that stopped in the directory's sector)
        img = bytearray(img)
        img[0x25000:0x25010] = bytes(16)
        self.assertEqual(fails(IP.plan(bytes(img), APP)), [])
        # a page that is not this area's: refused
        img[IP.SLOT + 0x1100] ^= 1
        self.assertEqual(fails(IP.plan(bytes(img), APP)), ["our sectors are in use: 0x42000..0x42fff"])
        # another image's pages: refused
        img = bytearray(stock_flash())
        img[IP.SLOT + 0x1000:IP.SLOT + 0x1100] = b"\x55" * 0x100
        self.assertEqual(fails(IP.plan(bytes(img), APP)), ["our sectors are in use: 0x42000..0x42fff"])

    def test_too_large(self):
        f = fails(IP.plan(stock_flash(), bytes(0x2B000)))
        self.assertEqual(len(f), 1)
        self.assertIn("at least 0x2000 bytes", f[0])

    def test_a_slot_off_the_key_grid(self):
        f = fails(IP.plan(stock_flash(), APP, at=0x45000))
        self.assertEqual(len(f), 1)
        self.assertIn("the two SFC key models encrypt it differently", f[0])

    def test_vm(self):
        img = stock_flash()
        self.assertIn("start after the area's last sector", fails(IP.plan(img, APP, vm=(IP.SLOT, 0x2000)))[0])
        for vm in ((0x68000, 0x8000), (0x7A000, 0x2000), (0x6C000, 0x4000)):
            f = fails(IP.plan(img, APP, vm=vm))
            self.assertEqual(len(f), 1, vm)
            self.assertIn("end by the stock update loader (0x6e000)", f[0])
        p = IP.plan(img, APP, vm=(0x50000, 0x4000))
        self.assertEqual(fails(p), [])
        self.assertEqual(p.rollback[1].erase, tuple(range(IP.CFG_AT, IP.CFG_AT + IP.CFG_SIZE, F.SECTOR))
                         + tuple(range(IP.SLOT, 0x54000, F.SECTOR)))

    def test_the_configuration_slots(self):
        img = bytearray(stock_flash())
        img[0x34000:0x34010] = bytes(16)
        self.assertEqual(fails(IP.plan(bytes(img), APP)), ["our sectors are in use: 0x34000..0x34fff"])
        p = IP.plan(stock_flash(), APP)
        self.assertEqual(p.rollback[1].erase[:16], tuple(range(0x25000, 0x35000, F.SECTOR)))
        self.assertEqual(p.rollback[1].erase[-1], 0x6D000)
        self.assertTrue(any(t.startswith("the configuration's two slots and the panel's state 0x25000..0x36fff")
                            for t in texts(p, "first run", "info")))
        # below the area, above the stock VM's sector
        self.assertTrue(any(t.startswith("the configuration's slots 0x25000..0x36fff below the area")
                            for t in texts(p, "ours", "pass")))
        # the firmware's layout (firmware/store/cfgstore.h) is the one planned here
        with open(os.path.join(IP.ROOT, "firmware", "store", "cfgstore.h")) as f:
            h = f.read()
        val = lambda n: int(re.search(r"#define %s (0x[0-9A-Fa-f]+|\d+)u" % n, h).group(1), 0)  # noqa: E731
        self.assertEqual(val("CFGSTORE_FLASH_AT"), IP.CFG_AT)
        with open(os.path.join(IP.ROOT, "firmware", "store", "statestore.h")) as f:
            sh = f.read()
        sectors = int(re.search(r"#define STATESTORE_SECTORS (\d+)u", sh).group(1))
        self.assertIn("#define STATESTORE_FLASH_AT (CFGSTORE_FLASH_AT + CFGSTORE_SLOTS * CFGSTORE_SLOT_SIZE)", sh)
        self.assertEqual(val("CFGSTORE_SLOTS") * val("CFGSTORE_SLOT_SIZE") + sectors * val("CFGSTORE_SECTOR"),
                         IP.CFG_SIZE)
        self.assertLessEqual(IP.CFG_AT + IP.CFG_SIZE, IP.SLOT)
        self.assertEqual(IP.VM_END, IP.LOADER_AT)


class Pieces(unittest.TestCase):
    def test_apply_is_nor_flash(self):
        img = bytes([0xF0]) + b"\xff" * (F.SECTOR * 2 - 1)
        self.assertEqual(IP.apply(img, [IP.Write("x", 0, b"\x0f")])[0], 0x00)  # programming only clears bits
        self.assertEqual(IP.apply(img, [IP.Write("x", 0, b"\x0f", (0,))])[0], 0x0F)
        self.assertEqual(IP.apply(img, [IP.Write("x", F.SECTOR, b"", (F.SECTOR,))]), img)
        self.assertEqual(IP.needs_erase(b"\xf0", b"\x0f"), 4)
        self.assertEqual(IP.needs_erase(b"\xff", b"\x0f"), 0)

    def test_broken_chains(self):
        after = bytearray(IP.plan(stock_flash(), APP).after)
        after[IP.SLOT + 0x500] ^= 1
        with self.assertRaisesRegex(ValueError, "CRC"):
            IP.boot_chain(bytes(after))
        nowhere = F.set_boot_pointer(after[:F.SECTOR], 0x30000) + bytes(after[F.SECTOR:])
        with self.assertRaisesRegex(ValueError, "no app_area_head at 0x30000"):
            IP.boot_chain(nowhere)
        odd = F.set_boot_pointer(after[:F.SECTOR], 0x30010) + bytes(after[F.SECTOR:])
        with self.assertRaisesRegex(ValueError, "not a sector"):
            IP.boot_chain(odd)

    def test_used(self):
        img = bytearray(b"\xff" * 0x10000)
        img[0x1000] = img[0x2FFF] = img[0x5000] = 0
        self.assertEqual(IP.used(bytes(img)), [(0x1000, 0x3000), (0x5000, 0x6000)])
        self.assertEqual(IP.used(bytes(img), 0x2000, 0x6000), [(0x2000, 0x3000), (0x5000, 0x6000)])

    def test_private(self):
        self.assertTrue(IP.private(os.path.join(IP.ROOT, "materials", "install")))
        self.assertTrue(IP.private(os.path.join(IP.ROOT, "build", "x")))
        self.assertFalse(IP.private(os.path.join(IP.ROOT, "docs")))
        self.assertFalse(IP.private(IP.ROOT))
        self.assertTrue(IP.private(tempfile.gettempdir()))


class Cli(unittest.TestCase):
    def run_main(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = IP.main(list(args))
        return code, out.getvalue(), err.getvalue()

    def test_files(self):
        img = stock_flash()
        with tempfile.TemporaryDirectory() as tmp:
            dump, app, out = (os.path.join(tmp, n) for n in ("dump.bin", "app.bin", "out"))
            with open(dump, "wb") as f:
                f.write(stock_read(img))
            with open(app, "wb") as f:
                f.write(APP)
            code, text, err = self.run_main(dump, "--app", app, "--no-cfg", "--out", out)
            self.assertEqual(code, 0, text + err)
            self.assertTrue(text.startswith("installation dry run: dump.bin (stock read), app.bin 9000 bytes, at 0x41000"))
            self.assertTrue(text.rstrip().endswith("0 failed"))
            p = IP.plan(img, APP)
            want = {"flash-before.bin": img, "flash-after.bin": p.after, "area-phys.bin": p.phys,
                    "sector0-after.bin": p.after[:F.SECTOR], "sector0-before.bin": img[:F.SECTOR]}
            self.assertEqual(sorted(os.listdir(out)), sorted(list(want) + ["plan.json", "plan.txt"]))
            for name, data in want.items():
                with open(os.path.join(out, name), "rb") as f:
                    self.assertEqual(f.read(), data, name)
            with open(os.path.join(out, "plan.json")) as f:
                j = json.load(f)
            self.assertEqual(j["failed"], 0)
            self.assertEqual([(w["offset"], w["size"], w["crc16"]) for w in j["writes"]],
                             [(IP.SLOT, len(p.phys), F.crc16(p.phys)), (0, F.SECTOR, F.crc16(p.after[:F.SECTOR]))])
            self.assertEqual(j["crc16"], {"before": F.crc16(img), "after": F.crc16(p.after)})

            code, text, _ = self.run_main(dump, "--app", app, "--no-cfg", "--json", "--at", "0x45000")
            self.assertEqual(code, 1)
            self.assertEqual(json.loads(text)["failed"], 1)

            inside = os.path.join(IP.ROOT, "docs", "install-test")
            code, _, err = self.run_main(dump, "--app", app, "--no-cfg", "--out", inside)
            self.assertEqual(code, 2)
            self.assertIn("SPEC C-7", err)
            self.assertFalse(os.path.exists(inside))

    def test_cfg_tool_required(self):
        # a plan without cfg_tool.bin makes an image whose every start resets (bringup.md, run 3)
        img = stock_flash()
        with tempfile.TemporaryDirectory() as tmp:
            dump, app, cfg = (os.path.join(tmp, n) for n in ("dump.bin", "app.bin", "cfg_tool.bin"))
            for path, data in ((dump, stock_read(img)), (app, APP), (cfg, b"\1" * 0x360)):
                with open(path, "wb") as f:
                    f.write(data)
            with self.assertRaises(SystemExit) as e:
                self.run_main(dump, "--app", app)
            self.assertEqual(e.exception.code, 2)
            with self.assertRaises(SystemExit) as e:
                self.run_main(dump, "--app", app, "--cfg", cfg, "--no-cfg")
            self.assertEqual(e.exception.code, 2)
            code, text, err = self.run_main(dump, "--app", app, "--cfg", cfg)
            self.assertEqual(code, 0, text + err)
            self.assertIn("cfg_tool.bin 0x", text)
            self.assertNotIn("whether our SDK build needs cfg_tool.bin", text)


@unittest.skipUnless(os.path.exists(DUMP), "the V012 dump is not here (materials/, local only)")
class Dump(unittest.TestCase):
    def test_the_v012_dump(self):
        with open(DUMP, "rb") as f:
            p = IP.plan(f.read(), APP)
        self.assertEqual(fails(p), [])
        self.assertEqual(p.view, "stock")
        self.assertIn("the update loader's sectors (0x6e000..0x7afff) stay outside our VM", texts(p, "ours", "pass"))
        self.assertTrue(texts(p, "first run", "pass")[0].startswith("BTIF item 102 (CFG_BT_MAC_ADDR) at 0x7b000: valid"))


if __name__ == "__main__":
    unittest.main()

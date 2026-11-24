"""Writes the flashes test_bootsel.c runs on, from tools/image's model (flashimg.py, installplan.py,
test_installplan.py's synthetic stock flash): M-Vave's flash as stored, then the same after the
installation plan (sector 0 pointing at our area), 512 KB each, one after the other.

  python3 -I firmware/tests/bootsel_vec.py OUT
  python3 -I firmware/tests/bootsel_vec.py --installed OUT   the second alone (devhost --boot)
"""
import os
import random
import sys

IMAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "image")
sys.path.insert(0, IMAGE)
import installplan as IP  # noqa: E402
import test_installplan as TIP  # noqa: E402

stock = TIP.stock_flash()
p = IP.plan(stock, TIP.APP, random.Random(5).randbytes(0x360))
assert not p.report.failed
installed = sys.argv[1] == "--installed"
with open(sys.argv[-1], "wb") as f:
    f.write(p.after if installed else stock + p.after)

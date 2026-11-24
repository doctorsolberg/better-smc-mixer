"""Unit tests for capture_report.py. Run: python3 -I tools/stage0/test_capture_report.py"""

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capture_report as cr  # noqa: E402

MS = 1_000_000


def rx(t_ms, hex_str, src="SMC-Mixer-Master"):
    return {"t_ns": t_ms * MS, "dir": "rx", "src": src, "hex": hex_str, "msg": ""}


def marker(t_ms, event, step, **kw):
    return dict({"t_ns": t_ms * MS, "dir": "marker", "event": event, "step": step}, **kw)


def lines(*recs):
    return [json.dumps(r) for r in recs]


class Decode(unittest.TestCase):
    def test_messages(self):
        self.assertEqual(cr.decode("B0 0E 7F")[:2], (("cc", 1, 14), 127))
        self.assertEqual(cr.decode("97 10 7F")[:2], (("note", 8, 16), 127))
        self.assertEqual(cr.decode("90 10 00")[:2], (("note", 1, 16), 0))
        self.assertEqual(cr.decode("80 10 40")[2], "off64")
        self.assertEqual(cr.decode("E2 7F 7F")[:2], (("pitch", 3, None), 16383))
        self.assertEqual(cr.decode("F0 00 59 F7")[:2], (("sysex", None, None), 4))


class Parse(unittest.TestCase):
    def test_messages_go_to_their_step(self):
        s = cr.parse(lines(
            {"t_ns": 0, "dir": "meta", "event": "scenario", "name": "t"},
            rx(5, "B0 01 01"),  # before any step
            marker(10, "step_begin", "a", index=1, text="press"),
            rx(20, "B0 0E 7F"),
            marker(30, "note", "a", text="LED lit"),
            rx(40, "B0 0E 00"),
            marker(50, "step_end", "a", index=1, result="done"),
            marker(55, "answer", "a", question="LEDs?", text="M1"),
            rx(60, "B0 01 02"),  # between steps
            marker(70, "step_begin", "b", index=2, text="turn"),
            marker(80, "step_end", "b", index=2, result="done"),
            {"t_ns": 90 * MS, "dir": "meta", "event": "stop", "how": "done"},
        ))
        self.assertEqual(s.scenario, "t")
        self.assertEqual(s.stop, "done")
        self.assertEqual([a.step for a in s.attempts], ["a", "b"])
        a = s.attempts[0]
        self.assertEqual([m[3] for m in a.msgs], [127, 0])
        self.assertEqual(a.notes, ["LED lit"])
        self.assertEqual(a.answers, [("LEDs?", "M1")])
        self.assertEqual([(o[0], o[4]) for o in s.outside], [(None, 1), ("a", 2)])
        self.assertEqual(s.attempts[1].msgs, [])

    def test_order_by_timestamp_not_by_line(self):
        # A message stamped before step_end but written after it still belongs to the step.
        s = cr.parse(lines(
            marker(10, "step_begin", "a", index=1, text=""),
            marker(50, "step_end", "a", index=1, result="done"),
            rx(49, "B0 0E 7F"),
        ))
        self.assertEqual(len(s.attempts[0].msgs), 1)
        self.assertEqual(s.outside, [])

    def test_redo_makes_two_attempts_and_answer_goes_to_last(self):
        s = cr.parse(lines(
            marker(10, "step_begin", "a", index=1, text=""),
            marker(20, "step_end", "a", index=1, result="redo"),
            marker(30, "step_begin", "a", index=1, text=""),
            marker(40, "step_end", "a", index=1, result="done"),
            marker(45, "answer", "a", question="q", text="x"),
        ))
        self.assertEqual([a.result for a in s.attempts], ["redo", "done"])
        self.assertEqual(s.attempts[0].answers, [])
        self.assertEqual(s.attempts[1].answers, [("q", "x")])

    def test_mirror_source_is_left_out(self):
        recs = [marker(0, "step_begin", "a", index=1, text="")]
        for t, h in [(10, "90 10 7F"), (20, "90 10 00")]:
            recs += [rx(t, h, "Dev-Private"), rx(t, h, "Dev-Master")]
        recs += [marker(50, "step_end", "a", index=1, result="done")]
        s = cr.parse(lines(*recs))
        self.assertEqual(s.mirrors, {"Dev-Master": ("Dev-Private", 2)})
        self.assertEqual([m[1] for m in s.attempts[0].msgs], ["Dev-Private", "Dev-Private"])
        text = "\n".join(cr.report(s))
        self.assertIn("mirror source left out: Dev-Master repeats Dev-Private", text)
        self.assertIn("note ch1 16 ×2:", text)

    def test_different_streams_are_both_kept(self):
        s = cr.parse(lines(
            marker(0, "step_begin", "a", index=1, text=""),
            rx(10, "90 10 7F", "A"), rx(10, "90 10 7F", "B"), rx(20, "90 10 00", "A"),
            marker(50, "step_end", "a", index=1, result="done"),
        ))
        self.assertEqual(s.mirrors, {})
        self.assertEqual(len(s.attempts[0].msgs), 3)

    def test_bad_lines_are_counted(self):
        s = cr.parse(["{not json", "[]", json.dumps({"dir": "rx"}), ""])
        self.assertEqual(s.bad_lines, 3)


class Report(unittest.TestCase):
    def test_fader_stats(self):
        recs = [marker(0, "step_begin", "fader", index=1, text="move")]
        recs += [rx(10 + i * 10, "B0 09 %02X" % (i * 2)) for i in range(40)]  # 0, 2, … 78
        recs += [marker(500, "step_end", "fader", index=1, result="done")]
        text = "\n".join(cr.report(cr.parse(lines(*recs))))
        self.assertIn("cc ch1 9 ×40", text)
        self.assertIn("min 0, max 78, 40 distinct", text)
        self.assertIn("smallest step between distinct values 2", text)
        self.assertIn("100 msg/s", text)

    def test_encoder_histogram_and_address_table(self):
        recs = [marker(0, "step_begin", "enc", index=1, text="turn")]
        recs += [rx(10 + i * 10, "B0 10 01") for i in range(10)]
        recs += [rx(200 + i * 10, "B0 10 41") for i in range(10)]
        recs += [marker(500, "step_end", "enc", index=1, result="done")]
        text = "\n".join(cr.report(cr.parse(lines(*recs))))
        self.assertIn("values: 1×10, 65×10", text)
        self.assertIn("repeated values 18", text)
        self.assertIn("addresses seen:", text)
        self.assertIn("enc ×20", text)

    def test_direction_changes(self):
        vals = list(range(10, 30)) + list(range(28, 9, -1))  # up, then down
        recs = [marker(0, "step_begin", "enc", index=1, text="turn")]
        recs += [rx(10 + i * 10, "B0 03 %02X" % v) for i, v in enumerate(vals)]
        recs += [marker(1000, "step_end", "enc", index=1, result="done")]
        text = "\n".join(cr.report(cr.parse(lines(*recs))))
        self.assertIn("direction changes 1, repeated values 0", text)

    def test_short_group_lists_every_message(self):
        recs = [
            marker(0, "step_begin", "m1", index=1, text="press"),
            rx(100, "90 10 7F"), rx(250, "80 10 00"),
            marker(500, "step_end", "m1", index=1, result="done"),
        ]
        text = "\n".join(cr.report(cr.parse(lines(*recs))))
        self.assertIn("note ch1 16 ×2: +0.100 on127, +0.250 off0", text)


if __name__ == "__main__":
    unittest.main()

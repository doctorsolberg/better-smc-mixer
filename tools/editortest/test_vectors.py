#!/usr/bin/env python3
"""The firmware's side of the editor's contract tests (docs/integration.md, T-1).

- The shared vectors (tools/editortest/vectors/) are what the firmware gives now: the core's
  messages for every control of the test, factory and generated configurations (build/host/smcsim),
  and the fields smcconf names in the broken files. A stale vector means one side changed: write
  them again (vectors.py --write) and run the editor's side (tools/env/test_editor.sh).
- The configurations the editor's code wrote (vectors/gen-N.json) are valid and canonical for
  smcconf and survive its binary form.

Standard library only; needs build/host/smcsim (make -C firmware sim).
"""
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vectors  # noqa: E402
from vectors import codec  # noqa: E402


class Vectors(unittest.TestCase):
    def test_vectors_are_current(self):
        self.assertTrue(os.access(vectors.SIM, os.X_OK), "make -C firmware sim")
        for name, text in vectors.expected().items():
            with self.subTest(vector=name):
                with open(os.path.join(vectors.VECTORS, name), encoding="utf-8") as f:
                    self.assertTrue(f.read() == text, "%s is stale: python3 -I tools/editortest/vectors.py "
                                    "--write, then tools/env/test_editor.sh" % name)

    def test_generated_files_are_valid_and_canonical(self):
        gen = vectors.generated()
        self.assertGreaterEqual(len(gen), 2)
        for name, path in gen:
            with self.subTest(file=name):
                with open(path, encoding="utf-8") as f:
                    text = f.read()
                m = codec.from_json(json.loads(text))
                self.assertEqual(codec.dumps(codec.to_json(m)), text, "not in smcconf's canonical form")
                blob = codec.encode(m)  # names stay in the file (D-8): the binary form round trips
                self.assertEqual(codec.encode(codec.decode(blob)), blob)

    def test_every_fault_is_refused(self):
        with open(os.path.join(vectors.VECTORS, "faults.json"), encoding="utf-8") as f:
            cases = json.load(f)
        self.assertGreater(len(cases), 40)
        for c in cases:
            self.assertTrue(c["path"] or c["file"].get("global_") is not None, c)


if __name__ == "__main__":
    unittest.main()

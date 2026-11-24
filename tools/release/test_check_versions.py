#!/usr/bin/env python3
"""Tests of check_versions.py: the repository agrees, and a planted mismatch in each place is found."""
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_versions as cv  # noqa: E402


def planted(path, text):
    """The file's text with its first version number raised by one."""
    if path == "VERSION" or path.endswith("version.js"):  # the product's: its patch number
        return re.sub(r"(\d+\.\d+\.)(\d+)", lambda m: m.group(1) + str(int(m.group(2)) + 1), text, count=1)
    if path.endswith("app_version.h"):
        return re.sub(r"(#define APP_VERSION_MINOR )(\d+)", lambda m: m.group(1) + str(int(m.group(2)) + 1),
                      text, count=1)
    if path.endswith(".json"):
        return re.sub(r'("(?:schemaVersion|binaryVersion|const|cfg_version|link_version)":\s*)(\d+)',
                      lambda m: m.group(1) + str(int(m.group(2)) + 1), text, count=1)
    return re.sub(r"(VERSION\D{0,4}|schemaVersion: |\"(?:cfg|link)_version\": )(\d+)",
                  lambda m: m.group(1) + str(int(m.group(2)) + 1), text, count=1)


class Versions(unittest.TestCase):
    def test_repository_agrees(self):
        self.assertEqual(cv.check(), [])

    def test_each_place_planted(self):
        for what, places in cv.PLACES.items():
            for path, _ in places:
                with self.subTest(what=what, path=path):
                    def read(p, path=path):
                        text = cv.read_file(p)
                        return planted(p, text) if p == path else text
                    self.assertNotEqual(planted(path, cv.read_file(path)), cv.read_file(path), "nothing planted")
                    problems = cv.check(read)
                    self.assertTrue(any(path in p for p in problems), problems)

    def test_missing_version(self):
        problems = cv.check(lambda p: "" if p.endswith("link.h") else cv.read_file(p))
        self.assertTrue(any("link.h" in p and "not found" in p for p in problems), problems)


if __name__ == "__main__":
    unittest.main()

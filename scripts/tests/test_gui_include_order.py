"""Windows needs GUI headers before libslic3r headers in the C++ unit tests.

The app's own GUI sources compile with a precompiled header that brings in <windows.h> (through wx)
before anything else. The unit tests in tests/slic3rutils/ don't use it, so on Windows a test that
includes libslic3r headers first and a slic3r/GUI header afterwards fails to compile inside wx or
boost/asio ("use of undeclared identifier 'CP_ACP'", "'RGB'", "'GetKeyState'"...). That broke both
Windows CI builds twice on 2026-09-26/27 (Build all runs 36243478905 and 36274830855); macOS and Linux
never show it. This check makes the rule fail on every platform, before CI.
"""
import os
import re
import unittest

TESTS_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "tests", "slic3rutils")
PROJECT_INCLUDE = re.compile(r'^\s*#\s*include\s+"((?:libslic3r|slic3r)/[^"]+)"')


def first_misordered_include(lines):
    """The first slic3r/GUI include that comes after a libslic3r include, or None."""
    seen_libslic3r = None
    for number, line in enumerate(lines, 1):
        match = PROJECT_INCLUDE.match(line)
        if not match:
            continue
        header = match.group(1)
        if header.startswith("libslic3r/") and seen_libslic3r is None:
            seen_libslic3r = (number, header)
        elif header.startswith("slic3r/GUI/") and seen_libslic3r is not None:
            return seen_libslic3r, (number, header)
    return None


class GuiHeadersComeFirst(unittest.TestCase):
    def test_rule_detects_the_bad_order(self):
        bad = ['#include "libslic3r/Model.hpp"', '#include "slic3r/GUI/PartPlate.hpp"']
        self.assertIsNotNone(first_misordered_include(bad))
        good = ['#include "slic3r/GUI/PartPlate.hpp"', '#include "libslic3r/Model.hpp"']
        self.assertIsNone(first_misordered_include(good))

    def test_every_slic3rutils_test_includes_gui_headers_first(self):
        problems = []
        for name in sorted(os.listdir(TESTS_DIR)):
            if not name.endswith((".cpp", ".hpp")):
                continue
            with open(os.path.join(TESTS_DIR, name), encoding="utf-8", errors="replace") as f:
                found = first_misordered_include(f.read().splitlines())
            if found:
                (lib_line, lib), (gui_line, gui) = found
                problems.append(f"{name}: {gui} (line {gui_line}) comes after {lib} (line {lib_line})")
        self.assertEqual(problems, [], "Include slic3r/GUI headers before libslic3r ones (Windows): " + "; ".join(problems))


if __name__ == "__main__":
    unittest.main()

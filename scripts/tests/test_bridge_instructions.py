#!/usr/bin/env python3
"""The bridge answers initialize itself, app or no app, so the server instructions a client shows are
the ones it reads from orcamcp_tools.json: the app's own, as the C++ registry generated them.

Claude Code shows a server's instructions before any tool schema is loaded, and an agent there picks
tools by name alone, so these must never be missing or differ from the app's.

Run from the repo root:  python3 -m unittest discover -s scripts/tests -t scripts
"""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bridge_test_support import load_bridge, load_manifest  # noqa: E402


def initialize(bridge):
    return bridge.handle_local_request({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})["result"]


class InstructionsTests(unittest.TestCase):
    def test_the_golden_file_carries_the_instructions(self):
        instructions = load_manifest().get("instructions")
        self.assertIsInstance(instructions, str)
        self.assertTrue(instructions)

    def test_initialize_answers_the_files_instructions(self):
        self.assertEqual(initialize(load_bridge())["instructions"], load_manifest()["instructions"])

    def test_initialize_answers_them_with_the_app_running_too(self):
        # handle_local_request answers initialize before it looks for the app at all.
        bridge = load_bridge()
        with mock.patch.object(bridge, "check_orcaslicer_connection", return_value=bridge.LIVE) as probe:
            result = initialize(bridge)
        self.assertEqual(result["instructions"], load_manifest()["instructions"])
        probe.assert_not_called()

    def test_a_file_without_instructions_still_initializes(self):
        manifest = load_manifest()
        del manifest["instructions"]
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "orcamcp_tools.json")
            with open(path, "w", encoding="utf-8") as f:
                json.dump(manifest, f)
            bridge = load_bridge()
            with contextlib.redirect_stderr(io.StringIO()):
                bridge.install_tools_manifest(path)
        result = initialize(bridge)
        self.assertNotIn("instructions", result)
        self.assertIn("serverInfo", result)

    def test_an_unreadable_file_still_initializes(self):
        bridge = load_bridge()
        with contextlib.redirect_stderr(io.StringIO()):
            bridge.install_tools_manifest(os.path.join(tempfile.gettempdir(), "no-such-orcamcp-tools.json"))
        result = initialize(bridge)
        self.assertNotIn("instructions", result)
        self.assertIn("serverInfo", result)

    def test_no_instruction_text_is_written_in_the_bridge(self):
        """The text lives in the C++ registry; the bridge only reads it."""
        with open(load_bridge().__file__, encoding="utf-8") as f:
            source = f.read()
        first_line = load_manifest()["instructions"].splitlines()[0]
        self.assertNotIn(first_line, source)


if __name__ == "__main__":
    unittest.main()

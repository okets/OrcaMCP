"""The bridge's tests see none of the shell's ORCAMCP_* settings: a test must not pass or fail on what the
caller happened to export. On 2026-09-28 `ORCAMCP_PORT=1 python3 -m unittest discover` failed nine tests,
because the pinned-port rule read the shell's ORCAMCP_PORT.

Run from the repo root:  python3 -m unittest discover -s scripts/tests -t scripts
"""

import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bridge_test_support import EMPTY_INSTANCES_DIR, load_bridge  # noqa: E402

SHELL = {"ORCAMCP_PORT": "1", "ORCAMCP_HOST": "example.invalid", "ORCAMCP_TIMEOUT": "5",
         "ORCAMCP_APP_PATH": "/opt/somewhere/OrcaSlicer", "ORCAMCP_INSTANCES_DIR": "/nowhere/instances",
         "ORCAMCP_DEBUG": "1"}


class TestEnvironmentTests(unittest.TestCase):
    def test_the_tests_bridge_ignores_an_orcamcp_port_the_shell_sets(self):
        with mock.patch.dict(os.environ, {"ORCAMCP_PORT": "1"}):
            bridge = load_bridge()
        self.assertIsNone(bridge.PINNED_PORT)
        self.assertNotIn(":1/", bridge.ORCAMCP_URL)

    def test_the_tests_bridge_ignores_every_setting_of_the_shell(self):
        with mock.patch.dict(os.environ, SHELL):
            bridge = load_bridge()
            with mock.patch.object(bridge.os.path, "isfile", side_effect=lambda path: path == SHELL["ORCAMCP_APP_PATH"]):
                self.assertIsNone(bridge.get_orcamcp_executable())  # read when start_orca runs, not at load
        self.assertIsNone(bridge.PINNED_PORT)
        self.assertEqual(bridge.TIMEOUT, 120)
        self.assertEqual(bridge.ORCAMCP_HOST, "127.0.0.1")
        self.assertEqual(bridge.INSTANCES_DIR, EMPTY_INSTANCES_DIR)
        self.assertFalse(bridge.ENV.get("ORCAMCP_DEBUG"))

    def test_a_test_that_needs_a_setting_passes_it(self):
        bridge = load_bridge(ORCAMCP_PORT="13625", ORCAMCP_TIMEOUT="30")
        self.assertEqual(bridge.PINNED_PORT, 13625)
        self.assertEqual(bridge.ORCAMCP_URL, "http://127.0.0.1:13625/mcp")
        self.assertEqual(bridge.TIMEOUT, 30)


if __name__ == "__main__":
    unittest.main()

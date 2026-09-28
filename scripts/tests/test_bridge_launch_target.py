"""start_orca launches only the installed OrcaMCP, or ORCAMCP_APP_PATH: never a build found in the source
folder, which would run on the user's real data folder (the user, 2026-09-28).

Run from the repo root:  python3 -m unittest discover -s scripts/tests -t scripts
"""

import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bridge_test_support import REPO_ROOT, load_bridge  # noqa: E402


class LaunchTargetTests(unittest.TestCase):
    def test_start_orca_never_launches_a_build_in_a_source_folder(self):
        """A build found in the repo would run on the user's real data folder (the user, 2026-09-28)."""
        bridge = load_bridge()
        with mock.patch.object(bridge.os.path, "isfile", side_effect=lambda path: path.startswith(REPO_ROOT)):
            self.assertIsNone(bridge.get_orcamcp_executable())

    def test_orcamcp_app_path_is_launched_when_set(self):
        bridge = load_bridge(ORCAMCP_APP_PATH="/opt/test/OrcaSlicer")
        with mock.patch.object(bridge.os.path, "isfile", return_value=True):
            self.assertEqual(bridge.get_orcamcp_executable(), "/opt/test/OrcaSlicer")


if __name__ == "__main__":
    unittest.main()

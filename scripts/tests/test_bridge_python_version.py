"""The bridge runs on Python 3.9. Connect runs it with the app's own Python (3.12), but the Linux AppImage
uses the system's, and people write entries by hand: macOS's own /usr/bin/python3 is 3.9, and until
2026-11-18 the bridge died there on import (a `str | None` annotation, evaluated at definition).

Run from the repo root:  python3 -m unittest discover -s scripts/tests -t scripts
"""

import ast
import json
import os
import shutil
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bridge_test_support import BRIDGE_PATH, EMPTY_INSTANCES_DIR  # noqa: E402

OLDEST_SUPPORTED = (3, 9)


def interpreter_version(path: str):
    """(major, minor) of the Python at `path`, or None when it does not run."""
    try:
        out = subprocess.run([path, "-I", "-c", "import sys; print(*sys.version_info[:2])"],
                             capture_output=True, text=True, timeout=10).stdout.split()
        return int(out[0]), int(out[1])
    except (OSError, subprocess.SubprocessError, ValueError, IndexError):
        return None


def older_python():
    """A Python 3.9 on this machine, to run the bridge on; None when there is none (most CI runners)."""
    for candidate in ("/usr/bin/python3", shutil.which("python3.9")):
        if candidate and interpreter_version(candidate) == OLDEST_SUPPORTED:
            return candidate
    return None


class PythonVersionTests(unittest.TestCase):
    def test_annotations_are_never_evaluated(self):
        """Every annotation stays text, so the 3.10 `X | None` form is safe on 3.9."""
        with open(BRIDGE_PATH, encoding="utf-8") as f:
            tree = ast.parse(f.read())
        first_statement = next(node for node in tree.body
                               if not (isinstance(node, ast.Expr) and isinstance(node.value, ast.Constant)))
        self.assertIsInstance(first_statement, ast.ImportFrom)
        self.assertEqual(first_statement.module, "__future__")
        self.assertIn("annotations", [alias.name for alias in first_statement.names])

    @unittest.skipIf(older_python() is None, "no Python 3.9 on this machine")
    def test_the_bridge_answers_initialize_on_python_3_9(self):
        request = {"jsonrpc": "2.0", "id": 1, "method": "initialize",
                   "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": "test", "version": "1"}}}
        environment = {key: value for key, value in os.environ.items() if not key.startswith("ORCAMCP_")}
        environment["ORCAMCP_INSTANCES_DIR"] = EMPTY_INSTANCES_DIR
        result = subprocess.run([older_python(), "-I", BRIDGE_PATH], input=json.dumps(request) + "\n",
                                capture_output=True, text=True, timeout=30, env=environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        answer = json.loads(result.stdout.splitlines()[0])
        self.assertEqual(answer["id"], 1)
        self.assertIn("protocolVersion", answer["result"])


if __name__ == "__main__":
    unittest.main()

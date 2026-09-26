"""Shared by the bridge's tests: a fresh copy of scripts/orcamcp-bridge.py, and the golden tool list
(scripts/orcamcp_tools.json) it serves.

Not a test module itself (its name does not match test*.py). Test files import it after putting this
directory on sys.path, which works whether unittest discovers them with `-t scripts` (as CI does)
or with this directory as the top level.
"""

import importlib.util
import json
import os

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SCRIPTS_DIR = os.path.join(REPO_ROOT, "scripts")
BRIDGE_PATH = os.path.join(SCRIPTS_DIR, "orcamcp-bridge.py")
TOOLS_FILE = os.path.join(SCRIPTS_DIR, "orcamcp_tools.json")


def load_bridge():
    """A fresh copy of the bridge module, with its module-level state at its initial values.

    The bridge's filename has a hyphen, so it cannot be imported by name.
    """
    spec = importlib.util.spec_from_file_location("orcamcp_bridge", BRIDGE_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_manifest(path=TOOLS_FILE):
    """The golden tool list, as the C++ registry generated it."""
    with open(path, encoding="utf-8") as f:
        return json.load(f)

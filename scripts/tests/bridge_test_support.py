"""Shared by the bridge's tests: a fresh copy of scripts/orcamcp-bridge.py, and the golden tool list
(scripts/orcamcp_tools.json) it serves.

Not a test module itself (its name does not match test*.py). Test files import it after putting this
directory on sys.path, which works whether unittest discovers them with `-t scripts` (as CI does)
or with this directory as the top level.
"""

import atexit
import importlib.util
import json
import os
import shutil
import socket
import tempfile

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SCRIPTS_DIR = os.path.join(REPO_ROOT, "scripts")
BRIDGE_PATH = os.path.join(SCRIPTS_DIR, "orcamcp-bridge.py")
TOOLS_FILE = os.path.join(SCRIPTS_DIR, "orcamcp_tools.json")


def closed_port() -> int:
    """A loopback port nothing listens on: bound for a moment by the OS's choice, then released."""
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


# An instance registry folder with nothing in it, shared by every bridge the tests load.
EMPTY_INSTANCES_DIR = tempfile.mkdtemp(prefix="orcamcp-bridge-tests-")
atexit.register(shutil.rmtree, EMPTY_INSTANCES_DIR, True)


def load_bridge():
    """A fresh copy of the bridge module, with its module-level state at its initial values.

    It sees no OrcaMCP instance of this machine: its registry folder is empty, and the address it looks
    at before choosing one is a port nothing listens on. A test that wants instances starts its own.

    The bridge's filename has a hyphen, so it cannot be imported by name.
    """
    spec = importlib.util.spec_from_file_location("orcamcp_bridge", BRIDGE_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.INSTANCES_DIR = EMPTY_INSTANCES_DIR
    module.ORCAMCP_URL = f"http://127.0.0.1:{closed_port()}/mcp"
    module.launch_process = refuse_to_launch
    return module


def refuse_to_launch(executable):
    """No test launches an app: start_orca would start the user's installed OrcaMCP, on their real data
    (2026-09-28, a test of the "already running" answer did). A test of the launch fakes it."""
    raise AssertionError(f"a bridge test tried to launch {executable}")


def load_manifest(path=TOOLS_FILE):
    """The golden tool list, as the C++ registry generated it."""
    with open(path, encoding="utf-8") as f:
        return json.load(f)

#!/usr/bin/env python3
"""The bridge agrees on an MCP protocol version as the app does (OrcaMCPProtocolVersions.hpp), from the
list the app wrote into orcamcp_tools.json: the version a client asks for when OrcaMCP speaks it,
otherwise the newest it speaks. Through v2.5.0.9 it echoed whatever a client asked, versions it had never
heard of included.

Run from the repo root:  python3 -m unittest discover -s scripts/tests -t scripts
"""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bridge_test_support import load_bridge, load_manifest  # noqa: E402


def initialize(bridge, **params):
    request = {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": params}
    return bridge.handle_local_request(request)["result"]["protocolVersion"]


def bridge_reading(manifest):
    """A bridge that read `manifest` as its orcamcp_tools.json."""
    bridge = load_bridge()
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "orcamcp_tools.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(manifest, f)
        with contextlib.redirect_stderr(io.StringIO()):
            bridge.install_tools_manifest(path)
    return bridge


class ProtocolVersionTests(unittest.TestCase):
    def test_the_golden_file_lists_the_versions_newest_first(self):
        versions = load_manifest().get("protocol_versions")
        self.assertIsInstance(versions, list)
        self.assertTrue(versions)
        self.assertTrue(all(isinstance(version, str) for version in versions))
        self.assertEqual(versions, sorted(versions, reverse=True))

    def test_a_version_orcamcp_speaks_is_answered_as_asked(self):
        bridge = load_bridge()
        for version in load_manifest()["protocol_versions"]:
            with self.subTest(version=version):
                self.assertEqual(initialize(bridge, protocolVersion=version), version)

    def test_any_other_is_answered_with_the_newest_orcamcp_speaks(self):
        bridge = load_bridge()
        newest = load_manifest()["protocol_versions"][0]
        for asked in ("2099-01-01", "1.0", 20250618, None, ["2025-06-18"]):
            with self.subTest(asked=asked):
                self.assertEqual(initialize(bridge, protocolVersion=asked), newest)
        self.assertEqual(initialize(bridge), newest)

    def test_the_versions_are_the_files(self):
        manifest = load_manifest()
        manifest["protocol_versions"] = ["2030-01-01", "2029-01-01"]
        bridge = bridge_reading(manifest)
        self.assertEqual(initialize(bridge, protocolVersion="2029-01-01"), "2029-01-01")
        self.assertEqual(initialize(bridge, protocolVersion="2025-06-18"), "2030-01-01")

    def test_a_file_without_them_still_initializes_with_the_oldest_version(self):
        manifest = load_manifest()
        del manifest["protocol_versions"]
        bridge = bridge_reading(manifest)
        self.assertEqual(initialize(bridge, protocolVersion="2025-06-18"), "2024-11-05")


if __name__ == "__main__":
    unittest.main()

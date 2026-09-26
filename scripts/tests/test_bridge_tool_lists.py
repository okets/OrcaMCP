#!/usr/bin/env python3
"""The bridge must serve the same tool list whether the app is running or not.

An agent framework fingerprints every tool's name and description when a person approves the
connection. The bridge answers tools/list from orcamcp_tools.json until the app is up and from the
app afterwards, so any difference between those two lists -- including the bridge's own start_orca,
which it used to define twice in two wordings -- reads as the toolset changing after review, and the
framework disconnects.

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
from bridge_test_support import BRIDGE_PATH, load_bridge, load_manifest  # noqa: E402


def simulated_live_response(bridge, manifest):
    """What the app's tools/list answers, if it is the build orcamcp_tools.json was generated from."""
    tools = [bridge.list_entry(t) for t in manifest["server_tools"]]
    return {"jsonrpc": "2.0", "id": 1, "result": {"tools": tools}}


def fingerprint(tools):
    return [(t["name"], t["description"]) for t in tools]


class OfflineAndOnlineListsTests(unittest.TestCase):
    def setUp(self):
        self.bridge = load_bridge()
        self.manifest = load_manifest()
        self.offline = self.bridge.get_full_tools_list()
        self.online = self.bridge.adopt_live_tools(simulated_live_response(self.bridge, self.manifest))["result"]["tools"]

    def test_offline_and_online_lists_have_the_same_names_and_descriptions(self):
        self.assertEqual(fingerprint(self.offline), fingerprint(self.online))

    def test_offline_and_online_lists_are_identical(self):
        self.assertEqual(self.offline, self.online)

    def test_start_orca_is_listed_once_either_way(self):
        for label, tools in (("offline", self.offline), ("online", self.online)):
            with self.subTest(list=label):
                self.assertEqual([t["name"] for t in tools].count("start_orca"), 1)

    def test_the_bridge_tools_come_first(self):
        bridge_names = [t["name"] for t in self.manifest["bridge_tools"]]
        self.assertEqual([t["name"] for t in self.offline[:len(bridge_names)]], bridge_names)

    def test_every_listed_tool_is_in_the_manifest(self):
        manifest_names = {t["name"] for t in self.manifest["server_tools"] + self.manifest["bridge_tools"]}
        self.assertEqual({t["name"] for t in self.offline}, manifest_names)

    def test_the_online_list_is_what_the_bridge_caches(self):
        self.assertEqual(self.bridge.CACHED_TOOLS, self.online)


class BridgeToolTextTests(unittest.TestCase):
    def setUp(self):
        self.bridge = load_bridge()
        self.bridge_tools = load_manifest()["bridge_tools"]
        with open(BRIDGE_PATH, encoding="utf-8") as f:
            self.bridge_source = f.read()

    def test_every_bridge_tool_has_a_handler_and_every_handler_a_tool(self):
        self.assertEqual(set(self.bridge.BRIDGE_HANDLERS), {t["name"] for t in self.bridge_tools})

    def test_no_bridge_tool_text_is_written_in_the_bridge(self):
        """The text lives in the C++ registry; the bridge only reads it."""
        for tool in self.bridge_tools:
            with self.subTest(tool=tool["name"]):
                self.assertNotIn(tool["description"], self.bridge_source)
                self.assertNotIn(tool["description"][:40], self.bridge_source)

    def test_start_orca_calls_are_answered_by_the_bridge(self):
        with mock.patch.object(self.bridge, "launch_orcamcp",
                               return_value={"success": True, "message": "started"}) as launch:
            response = self.bridge.handle_local_request(
                {"jsonrpc": "2.0", "id": 7, "method": "tools/call", "params": {"name": "start_orca"}})
        launch.assert_called_once()
        self.assertEqual(response["id"], 7)
        self.assertFalse(response["result"]["isError"])


# Every way the golden file can be unusable. None of them may stop the bridge from starting, or from
# offering start_orca: without it an agent cannot even launch the app to get a working list.
BROKEN_MANIFESTS = {
    "missing file": None,
    "not JSON": "{not json",
    "not an object": "[]",
    "no bridge_tools": json.dumps({"server_tools": []}),
    "entry without a description": json.dumps({
        "server_tools": [{"name": "get_scene_info", "inputSchema": {"type": "object"}}], "bridge_tools": []}),
    "entry with a numeric name": json.dumps({
        "server_tools": [{"name": 7, "description": "x", "inputSchema": {"type": "object"}}], "bridge_tools": []}),
    "entry whose schema is not an object": json.dumps({
        "server_tools": [], "bridge_tools": [{"name": "start_orca", "description": "x", "inputSchema": "none"}]}),
    "entry that is not an object": json.dumps({"server_tools": ["get_scene_info"], "bridge_tools": []}),
}


class BrokenManifestTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def bridge_with(self, content):
        """A bridge that read `content` as its tool list (None: the file does not exist)."""
        path = os.path.join(self.tmp.name, "orcamcp_tools.json")
        if content is not None:
            with open(path, "w", encoding="utf-8") as f:
                f.write(content)
        bridge = load_bridge()
        with contextlib.redirect_stderr(io.StringIO()):
            bridge.install_tools_manifest(path)
        return bridge, path

    def offline_tools(self, bridge):
        with mock.patch.object(bridge, "check_orcaslicer_connection", return_value=bridge.DOWN):
            return bridge.handle_local_request({"jsonrpc": "2.0", "id": 1, "method": "tools/list"})["result"]["tools"]

    def test_loading_a_broken_file_never_raises(self):
        for label, content in BROKEN_MANIFESTS.items():
            with self.subTest(case=label):
                bridge, path = self.bridge_with(content)
                self.assertIn(path, bridge.TOOLS_MANIFEST_ERROR)
                self.assertEqual(bridge.OFFLINE_SERVER_TOOLS, [])

    def test_start_orca_is_still_listed_while_the_app_is_down(self):
        for label, content in BROKEN_MANIFESTS.items():
            with self.subTest(case=label):
                bridge, path = self.bridge_with(content)
                tools = self.offline_tools(bridge)
                self.assertEqual([t["name"] for t in tools], ["start_orca"])
                # It says what is wrong, so the agent can tell the user.
                self.assertIn(path, tools[0]["description"])
                self.assertEqual(tools[0]["inputSchema"]["type"], "object")

    def test_start_orca_is_still_listed_once_the_app_is_up(self):
        live = {"jsonrpc": "2.0", "id": 1, "result": {"tools": [
            {"name": "get_scene_info", "description": "Scene.", "inputSchema": {"type": "object", "properties": {}}}]}}
        bridge, _ = self.bridge_with(None)
        tools = bridge.adopt_live_tools(live)["result"]["tools"]
        self.assertEqual([t["name"] for t in tools], ["start_orca", "get_scene_info"])
        # ...and stays listed when the app quits again, since that list is what gets cached.
        self.assertEqual([t["name"] for t in bridge.CACHED_TOOLS], ["start_orca", "get_scene_info"])

    def test_start_orca_still_launches_the_app(self):
        bridge, _ = self.bridge_with(None)
        with mock.patch.object(bridge, "launch_orcamcp", return_value={"success": True, "message": "started"}):
            response = bridge.handle_local_request(
                {"jsonrpc": "2.0", "id": 5, "method": "tools/call", "params": {"name": "start_orca"}})
        self.assertFalse(response["result"]["isError"])

    def test_a_readable_file_leaves_no_error(self):
        bridge = load_bridge()
        self.assertIsNone(bridge.TOOLS_MANIFEST_ERROR)
        self.assertEqual([t["name"] for t in bridge.BRIDGE_TOOLS],
                         [t["name"] for t in load_manifest()["bridge_tools"]])


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""scripts/orcamcp_tools.json: the tool list the bridge serves, generated from the app's registry.

The C++ test tests/slic3rutils/test_mcp_tool_list.cpp fails whenever the file and the registry
disagree, and it needs no running app, so that is the drift guard CI relies on. These tests pin the
file's shape and a few entries that were once found stale in the field. The live comparison at the
end is an extra check against whatever app is running; it skips when none is.

Run from the repo root:  python3 -m unittest discover -s scripts/tests -t scripts
"""

import json
import os
import sys
import unittest
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bridge_test_support import load_manifest  # noqa: E402


def all_tools(manifest):
    return manifest["server_tools"] + manifest["bridge_tools"]


def server_tools_by_name():
    return {t["name"]: t for t in load_manifest()["server_tools"]}


def live_server_tools():
    url = f"http://{os.environ.get('ORCAMCP_HOST', 'localhost')}:{os.environ.get('ORCAMCP_PORT', '13618')}/mcp"
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {}}).encode()
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=2) as resp:
            return json.load(resp)["result"]["tools"]
    except Exception:
        return None


class ManifestShapeTests(unittest.TestCase):
    def setUp(self):
        self.manifest = load_manifest()

    def test_it_lists_app_tools_and_bridge_tools(self):
        self.assertGreaterEqual(len(self.manifest["server_tools"]), 79)
        self.assertIn("start_orca", [t["name"] for t in self.manifest["bridge_tools"]])

    def test_every_name_is_listed_once(self):
        names = [t["name"] for t in all_tools(self.manifest)]
        self.assertEqual(len(names), len(set(names)))

    def test_each_list_is_sorted_by_name(self):
        for key in ("server_tools", "bridge_tools"):
            names = [t["name"] for t in self.manifest[key]]
            with self.subTest(list=key):
                self.assertEqual(names, sorted(names))

    def test_every_tool_has_a_category_a_summary_a_description_and_a_schema(self):
        # Which categories exist and how long a summary may be are the registry's rules, checked
        # by test_mcp_tool_list.cpp; here only that every field the bridge and agents read is there.
        for tool in all_tools(self.manifest):
            with self.subTest(tool=tool["name"]):
                self.assertTrue(tool["category"])
                self.assertTrue(tool["summary"])
                self.assertTrue(tool["description"])
                self.assertEqual(tool["inputSchema"]["type"], "object")
                self.assertIn("properties", tool["inputSchema"])
                self.assertIn("required", tool["inputSchema"])


class StaleEntryTests(unittest.TestCase):
    def test_get_presets_carries_its_filters(self):
        """The one entry that was found stale in the field, pinned so it cannot silently go back."""
        entry = server_tools_by_name()["get_presets"]
        self.assertEqual(set(entry["inputSchema"]["properties"]),
                         {"type", "vendor", "name_contains", "summary", "limit"})


class ValueSchemaAcceptsLists(unittest.TestCase):
    """A `value` that advertises "type": "string" is a contract the handler does not keep.

    apply_config, set_object_config and set_object_layer_range all shape a JSON value from the
    option's declared type (config_value_to_string), so a list-typed key takes an array. A
    schema-validating client rejects that array before the bridge ever sees it, and this file is
    the copy such a client is served while OrcaSlicer is still starting.
    """

    def value_schemas(self):
        by_name = server_tools_by_name()
        object_config = by_name["set_object_config"]["inputSchema"]["properties"]
        layer_range = by_name["set_object_layer_range"]["inputSchema"]["properties"]
        return {
            "set_object_config.settings": object_config["settings"]["items"]["properties"]["value"],
            "set_object_config.configs": object_config["configs"]["items"]["properties"]["settings"]["items"]["properties"]["value"],
            "set_object_layer_range.settings": layer_range["settings"]["items"]["properties"]["value"],
        }

    def test_value_is_not_constrained_to_a_string(self):
        for where, schema in self.value_schemas().items():
            with self.subTest(where=where):
                self.assertNotIn("type", schema)

    def test_key_is_still_constrained_to_a_string(self):
        by_name = server_tools_by_name()
        for name in ("set_object_config", "set_object_layer_range"):
            key = by_name[name]["inputSchema"]["properties"]["settings"]["items"]["properties"]["key"]
            with self.subTest(tool=name):
                self.assertEqual(key["type"], "string")


class RunningServerTests(unittest.TestCase):
    """Compares the file with the app on port 13618, if one is running. A different build than
    this checkout answers differently; that is the case the bridge's list_changed notice is for."""

    def setUp(self):
        self.live = live_server_tools()
        if self.live is None:
            self.skipTest("OrcaMCP server not reachable")
        self.static = server_tools_by_name()
        self.live_by_name = {t["name"]: t for t in self.live}

    def test_same_names(self):
        self.assertEqual(set(self.static), set(self.live_by_name),
                         "the running app and orcamcp_tools.json list different tools")

    def test_same_descriptions_and_schemas(self):
        for name in set(self.static) & set(self.live_by_name):
            with self.subTest(tool=name):
                self.assertEqual(self.static[name]["description"], self.live_by_name[name]["description"])
                self.assertEqual(self.static[name]["inputSchema"], self.live_by_name[name]["inputSchema"])


if __name__ == "__main__":
    unittest.main()

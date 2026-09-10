import json, os, sys, unittest, urllib.request, importlib
import pytest

SCRIPTS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, SCRIPTS)

def _load_static():
    mod = importlib.import_module("tools_schema")
    importlib.reload(mod)
    return mod.FULL_TOOLS_LIST

def _server_tools():
    url = f"http://{os.environ.get('ORCAMCP_HOST','localhost')}:{os.environ.get('ORCAMCP_PORT','13618')}/mcp"
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {}}).encode()
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=2) as resp:
            return json.load(resp)["result"]["tools"]
    except Exception:
        return None

def test_static_list_has_required_fields():
    tools = _load_static()
    assert len(tools) >= 50
    names = [t["name"] for t in tools]
    assert len(names) == len(set(names)), "duplicate tool names"
    for t in tools:
        assert t["description"], t["name"]
        assert t["inputSchema"]["type"] == "object", t["name"]
        assert "properties" in t["inputSchema"], t["name"]

def test_static_list_carries_get_presets_filters():
    """The one entry that was found stale in the field, pinned so it cannot silently go back.

    test_static_list_matches_running_server catches every drift but skips when nothing is
    listening -- which is exactly the situation in which this file is edited.
    """
    entry = next(t for t in _load_static() if t["name"] == "get_presets")
    assert set(entry["inputSchema"]["properties"]) == {
        "type", "vendor", "name_contains", "summary", "limit"
    }

def test_static_list_matches_running_server():
    live = _server_tools()
    if live is None:
        pytest.skip("OrcaMCP server not reachable")
    static = {t["name"]: t for t in _load_static()}
    live_map = {t["name"]: t for t in live}
    assert set(static) == set(live_map), f"drift: run scripts/regen_tools_schema.py; diff={set(static) ^ set(live_map)}"
    for name, tool in live_map.items():
        assert static[name]["inputSchema"] == tool["inputSchema"], name


class ValueSchemaAcceptsLists(unittest.TestCase):
    """A `value` that advertises "type": "string" is a contract the handler does not keep.

    apply_config, set_object_config and set_object_layer_range all shape a JSON value from the
    option's declared type (config_value_to_string), so a list-typed key takes an array. A
    schema-validating client rejects that array before the bridge ever sees it, and this file is
    the copy such a client is served while OrcaSlicer is still starting. A unittest.TestCase, not
    a bare function, because `python3 -m unittest discover` is the command that gates this repo's
    Python changes and it collects only TestCase subclasses.
    """

    def value_schemas(self):
        by_name = {t["name"]: t for t in _load_static()}
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
        by_name = {t["name"]: t for t in _load_static()}
        for name in ("set_object_config", "set_object_layer_range"):
            key = by_name[name]["inputSchema"]["properties"]["settings"]["items"]["properties"]["key"]
            with self.subTest(tool=name):
                self.assertEqual(key["type"], "string")

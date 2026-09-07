import json, os, sys, urllib.request, importlib
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

def test_static_list_matches_running_server():
    live = _server_tools()
    if live is None:
        pytest.skip("OrcaMCP server not reachable")
    static = {t["name"]: t for t in _load_static()}
    live_map = {t["name"]: t for t in live}
    assert set(static) == set(live_map), f"drift: run scripts/regen_tools_schema.py; diff={set(static) ^ set(live_map)}"
    for name, tool in live_map.items():
        assert static[name]["inputSchema"] == tool["inputSchema"], name

#!/usr/bin/env python3
"""The bridge's own tools, start_orca and wait_for_slice, refuse arguments their schema does not allow.

The schemas are the ones scripts/orcamcp_tools.json gives them, generated from the app's registry; the
refusal is JSON-RPC -32602, worded as the app words it for its own tools (OrcaMCPToolArguments.cpp,
tested in tests/slic3rutils/test_mcp_tool_arguments.cpp).

Run from the repo root:  python3 -m unittest discover -s scripts/tests -t scripts
"""

import json
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bridge_test_support import load_bridge, load_manifest  # noqa: E402

INVALID_PARAMS = -32602
ABSENT = object()  # a call that carries no "arguments" member at all
DONE_REPORT = {"status": "success", "outcome": "done", "timed_out": False}


def call(bridge, name, arguments=ABSENT):
    """A tools/call of `name`, as a client makes it, answered by the bridge."""
    params = {"name": name}
    if arguments is not ABSENT:
        params["arguments"] = arguments
    return bridge.handle_local_request({"jsonrpc": "2.0", "id": 5, "method": "tools/call", "params": params})


class BridgeToolArgumentTests(unittest.TestCase):
    def setUp(self):
        self.bridge = load_bridge()

    def assert_refused(self, response, message):
        self.assertEqual(response["id"], 5)
        self.assertEqual(response["error"]["code"], INVALID_PARAMS)
        self.assertEqual(response["error"]["message"], message)

    def test_wait_for_slice_refuses_an_argument_it_does_not_take_without_polling(self):
        with mock.patch.object(self.bridge, "call_app_tool") as poll:
            response = call(self.bridge, "wait_for_slice", {"timeout": 5})
        poll.assert_not_called()
        self.assert_refused(response, 'wait_for_slice has no argument "timeout". Its arguments: timeout_s.')

    def test_wait_for_slice_still_takes_timeout_s(self):
        with mock.patch.object(self.bridge, "run_wait_for_slice", return_value=dict(DONE_REPORT)) as wait:
            response = call(self.bridge, "wait_for_slice", {"timeout_s": 1})
        wait.assert_called_once_with(1.0)
        self.assertFalse(response["result"]["isError"])

    def test_start_orca_refuses_an_argument_and_launches_nothing(self):
        with mock.patch.object(self.bridge, "launch_orcamcp") as launch:
            response = call(self.bridge, "start_orca", {"path": "/Applications/OrcaMCP.app"})
        launch.assert_not_called()
        self.assert_refused(response, 'start_orca has no argument "path". Its arguments: new_instance.')

    def test_arguments_that_are_not_an_object_are_refused(self):
        for arguments, kind in (([5], "array"), ("5", "string"), (5, "number"), (True, "boolean")):
            with self.subTest(arguments=arguments):
                self.assert_refused(call(self.bridge, "wait_for_slice", arguments),
                                    f"wait_for_slice's arguments must be a JSON object of named arguments; got {kind}.")

    def test_absent_or_null_arguments_are_no_arguments(self):
        for arguments in (ABSENT, None, {}):
            with self.subTest(arguments=arguments), \
                    mock.patch.object(self.bridge, "launch_orcamcp",
                                      return_value={"success": True, "message": "started"}) as launch:
                response = call(self.bridge, "start_orca", arguments)
                launch.assert_called_once()
                self.assertFalse(response["result"]["isError"])

    def test_the_schemas_come_from_the_tool_list_file(self):
        """A property the file declares is taken, and none is written into the bridge itself."""
        manifest = load_manifest()
        for tool in manifest["bridge_tools"]:
            if tool["name"] == "wait_for_slice":
                tool["inputSchema"]["properties"]["poll_s"] = {"type": "number", "description": "Only in this test"}
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "orcamcp_tools.json")
            with open(path, "w", encoding="utf-8") as f:
                json.dump(manifest, f)
            self.bridge.install_tools_manifest(path)
        with mock.patch.object(self.bridge, "run_wait_for_slice", return_value=dict(DONE_REPORT)):
            response = call(self.bridge, "wait_for_slice", {"poll_s": 1})
        self.assertIn("result", response)

    def test_the_wording_is_the_apps(self):
        """The same sentences the app's check writes (test_mcp_tool_arguments.cpp), for what a bridge tool
        could be refused for."""
        schema = {"type": "object", "properties": {"a": {"type": "number"}, "b": {"type": "number"}},
                  "required": ["a"], "additionalProperties": False}
        self.assertEqual(self.bridge.argument_error("t", schema, {"x": 1, "y": 2}),
                         't has no arguments "x", "y" and is missing its required argument "a". Its arguments: a, b.')
        self.assertEqual(self.bridge.argument_error("t", schema, {"b": 1}),
                         't is missing its required argument "a". Its arguments: a, b.')
        self.assertIsNone(self.bridge.argument_error("t", schema, {"a": 1}))

    def test_no_bridge_tool_takes_a_nested_object(self):
        """The bridge checks its tools' arguments at the top level only, which is all they have: a nested
        object or list would need the app's walk (OrcaMCPToolArguments.cpp) here too."""
        for tool in load_manifest()["bridge_tools"]:
            for name, schema in tool["inputSchema"].get("properties", {}).items():
                with self.subTest(tool=tool["name"], argument=name):
                    self.assertNotIn(schema.get("type"), ("object", "array"))
                    self.assertNotIn("properties", schema)
                    self.assertNotIn("items", schema)



class WindowsPathNormalizationTests(unittest.TestCase):
    """On Windows the bridge rewrites the path arguments of every tool call it forwards. What is not
    named arguments it forwards untouched, so the app answers it with -32602 rather than the bridge
    failing the call with -32603."""

    def setUp(self):
        self.bridge = load_bridge()
        windows = mock.patch("platform.system", return_value="Windows")
        windows.start()
        self.addCleanup(windows.stop)

    @staticmethod
    def request(arguments):
        return {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "load_model", "arguments": arguments}}

    def test_a_path_argument_gets_backslashes(self):
        request = self.bridge.normalize_paths_for_windows(self.request({"file_path": "C:/Models/cube.stl"}))
        self.assertEqual(request["params"]["arguments"]["file_path"], "C:\\Models\\cube.stl")

    def test_arguments_that_are_not_an_object_are_forwarded_untouched(self):
        for arguments in ("file_path", ["path"], 5, None):
            with self.subTest(arguments=arguments):
                request = self.bridge.normalize_paths_for_windows(self.request(arguments))
                self.assertEqual(request["params"]["arguments"], arguments)
        request = {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": ["load_model", "path"]}
        self.assertEqual(self.bridge.normalize_paths_for_windows(request)["params"], ["load_model", "path"])

    def test_the_app_answers_them_with_its_invalid_params_error(self):
        refusal = {"jsonrpc": "2.0", "id": 3, "error": {"code": INVALID_PARAMS, "message": "from the app"}}
        with mock.patch.object(self.bridge, "post_to_app", return_value=refusal) as post:
            response = self.bridge.send_request(self.request("output_path"))
        self.assertEqual(post.call_args.args[0]["params"]["arguments"], "output_path")
        self.assertEqual(response["error"]["code"], INVALID_PARAMS)



class ToolWaitCapTests(unittest.TestCase):
    """Every tools/call the bridge forwards tells the app, in params._meta, how long a tool may wait for a
    job it starts (arrange_objects, auto_orient, flatten_object, clone_object): wait_for_slice's cap, so
    the call is answered before ORCAMCP_TIMEOUT."""

    def setUp(self):
        self.bridge = load_bridge()

    def forwarded(self, request):
        with mock.patch.object(self.bridge, "post_to_app", return_value={"jsonrpc": "2.0", "id": 1, "result": {}}) as post:
            self.bridge.send_request(request)
        return post.call_args.args[0]

    def test_a_tool_call_carries_the_cap(self):
        sent = self.forwarded({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                               "params": {"name": "arrange_objects", "arguments": {}}})
        self.assertEqual(sent["params"]["_meta"]["orcamcp/wait_cap_s"], self.bridge.wait_for_slice_cap())
        self.assertEqual(sent["params"]["arguments"], {})

    def test_the_client_s_own_meta_is_kept(self):
        sent = self.forwarded({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                               "params": {"name": "auto_orient", "_meta": {"progressToken": 7}}})
        self.assertEqual(sent["params"]["_meta"]["progressToken"], 7)
        self.assertIn("orcamcp/wait_cap_s", sent["params"]["_meta"])

    def test_a_meta_that_is_not_an_object_is_replaced_by_one_with_the_cap(self):
        """Otherwise the app, finding no cap, would wait its own 105 s past a shorter ORCAMCP_TIMEOUT."""
        for meta in ("x", [1], 5, None):
            with self.subTest(meta=meta):
                sent = self.forwarded({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                       "params": {"name": "auto_orient", "_meta": meta}})
                self.assertEqual(sent["params"]["_meta"], {"orcamcp/wait_cap_s": self.bridge.wait_for_slice_cap()})

    def test_a_timeout_too_short_to_wait_in_sends_no_wait(self):
        self.bridge.TIMEOUT = 1
        sent = self.forwarded({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": "auto_orient"}})
        self.assertEqual(sent["params"]["_meta"]["orcamcp/wait_cap_s"], 0)

    def test_other_methods_are_sent_as_they_are(self):
        sent = self.forwarded({"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {}})
        self.assertNotIn("_meta", sent["params"])


if __name__ == "__main__":
    unittest.main()

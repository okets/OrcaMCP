#!/usr/bin/env python3
"""Tests for the liveness verdict in scripts/orcamcp-bridge.py (stdlib unittest, no deps).

Run from the repo root:  python3 -m unittest discover -s scripts/tests -p 'test_bridge*.py' -v

The bug these pin (T3): eight concurrent calls, six answered, the last two came back
"OrcaMCP is not running" while the process was alive. HttpServer runs ONE io thread
(HttpServer.cpp:210) and every handler blocks it inside run_on_main_thread, so a burst
serialises and a 0.3s probe cannot be answered. Treating that as "down" -- and caching
it for 3 seconds -- is what fabricated the verdict.
"""

import importlib.util
import os
import socket
import sys
import unittest
import urllib.error
from unittest import mock

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BRIDGE_PATH = os.path.join(REPO_ROOT, "scripts", "orcamcp-bridge.py")


def load_bridge():
    """The bridge's filename has a hyphen, so it cannot be imported by name."""
    scripts_dir = os.path.join(REPO_ROOT, "scripts")
    if scripts_dir not in sys.path:
        sys.path.insert(0, scripts_dir)
    spec = importlib.util.spec_from_file_location("orcamcp_bridge", BRIDGE_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FakeResponse:
    def __init__(self, status=200):
        self.status = status

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False


class LivenessVerdictTests(unittest.TestCase):
    def setUp(self):
        self.bridge = load_bridge()
        self.bridge._connection_cache = {"connected": None, "last_check": 0}

    def verdict(self, side_effect):
        with mock.patch.object(self.bridge.urllib.request, "urlopen", side_effect=side_effect):
            return self.bridge.check_orcaslicer_connection(use_cache=False)

    def test_a_200_is_live(self):
        self.assertEqual(self.verdict(lambda *a, **k: FakeResponse(200)), self.bridge.LIVE)

    def test_an_http_error_is_live_because_something_answered(self):
        error = urllib.error.HTTPError(self.bridge.ORCAMCP_URL, 500, "boom", {}, None)
        # HTTPError substitutes a BytesIO for fp even when constructed with fp=None; leaving it
        # unclosed triggers a ResourceWarning at GC time ("Implicitly cleaning up <HTTPError ...>").
        self.addCleanup(error.close)
        self.assertEqual(self.verdict(error), self.bridge.LIVE)

    def test_a_timeout_is_busy_not_down(self):
        self.assertEqual(self.verdict(socket.timeout("timed out")), self.bridge.BUSY)
        self.assertEqual(self.verdict(TimeoutError("timed out")), self.bridge.BUSY)
        self.assertEqual(
            self.verdict(urllib.error.URLError(socket.timeout("timed out"))), self.bridge.BUSY
        )

    def test_a_refused_connection_is_down(self):
        refused = urllib.error.URLError(ConnectionRefusedError(61, "Connection refused"))
        self.assertEqual(self.verdict(refused), self.bridge.DOWN)

    def test_an_unresolvable_host_is_down(self):
        self.assertEqual(
            self.verdict(urllib.error.URLError(socket.gaierror(8, "nodename nor servname"))),
            self.bridge.DOWN,
        )

    def test_an_unrecognised_failure_is_busy_because_it_is_not_proof_of_death(self):
        self.assertEqual(self.verdict(urllib.error.URLError("something else")), self.bridge.BUSY)


class VerdictCachingTests(unittest.TestCase):
    def setUp(self):
        self.bridge = load_bridge()
        self.bridge._connection_cache = {"connected": None, "last_check": 0}

    def test_busy_is_never_cached(self):
        # The 3-second cache is what turned one timed-out probe into a run of false verdicts.
        with mock.patch.object(
            self.bridge.urllib.request, "urlopen", side_effect=socket.timeout("timed out")
        ):
            self.bridge.check_orcaslicer_connection(use_cache=True)
        self.assertIsNone(self.bridge._connection_cache["connected"])

    def test_live_and_down_are_cached(self):
        with mock.patch.object(
            self.bridge.urllib.request, "urlopen", side_effect=lambda *a, **k: FakeResponse(200)
        ):
            self.bridge.check_orcaslicer_connection(use_cache=True)
        self.assertEqual(self.bridge._connection_cache["connected"], self.bridge.LIVE)

        # DOWN is cached too, so a dead app is not re-probed on every single call.
        self.bridge._connection_cache = {"connected": None, "last_check": 0}
        refused = urllib.error.URLError(ConnectionRefusedError(61, "Connection refused"))
        with mock.patch.object(self.bridge.urllib.request, "urlopen", side_effect=refused):
            self.bridge.check_orcaslicer_connection(use_cache=True)
        self.assertEqual(self.bridge._connection_cache["connected"], self.bridge.DOWN)


class NotRunningAdviceTests(unittest.TestCase):
    def setUp(self):
        self.bridge = load_bridge()
        self.bridge._connection_cache = {"connected": None, "last_check": 0}
        self.bridge.CACHED_TOOLS = [{"name": "get_scene_info"}]

    def call_get_scene_info(self):
        return self.bridge.handle_local_request(
            {"jsonrpc": "2.0", "id": 7, "method": "tools/call",
             "params": {"name": "get_scene_info", "arguments": {}}}
        )

    def test_a_busy_server_gets_the_request_forwarded(self):
        with mock.patch.object(self.bridge, "check_orcaslicer_connection", return_value=self.bridge.BUSY):
            self.assertIsNone(self.call_get_scene_info())

    def test_a_live_server_gets_the_request_forwarded(self):
        with mock.patch.object(self.bridge, "check_orcaslicer_connection", return_value=self.bridge.LIVE):
            self.assertIsNone(self.call_get_scene_info())

    def test_only_a_down_server_is_told_to_start(self):
        with mock.patch.object(self.bridge, "check_orcaslicer_connection", return_value=self.bridge.DOWN):
            response = self.call_get_scene_info()
        self.assertIn("not running", response["result"]["content"][0]["text"])


class SendRequestAdviceTests(unittest.TestCase):
    def setUp(self):
        self.bridge = load_bridge()

    def message(self, side_effect):
        with mock.patch.object(self.bridge.urllib.request, "urlopen", side_effect=side_effect):
            response = self.bridge.send_request({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                                 "params": {"name": "slice_all", "arguments": {}}})
        return response["error"]["message"]

    def test_a_timeout_says_busy_and_names_the_timeout_knob(self):
        message = self.message(socket.timeout("timed out"))
        self.assertIn("ORCAMCP_TIMEOUT", message)
        self.assertNotIn("Is OrcaSlicer running", message)

    def test_a_refused_connection_says_it_is_not_running(self):
        message = self.message(urllib.error.URLError(ConnectionRefusedError(61, "Connection refused")))
        self.assertIn("not running", message)


if __name__ == "__main__":
    unittest.main()

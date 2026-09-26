"""The bridge's requests pass the app's request guard (src/slic3r/GUI/OrcaMCP/OrcaMCPRequestGuard.hpp).

The app refuses, with 403, any MCP request that carries an Origin header (a web page's) or whose Host
names anything but 127.0.0.1, localhost or [::1] on its port (DNS rebinding). The bridge must send
neither, or every tool call would fail. These tests point the bridge at a local server that records
what arrives.
"""

import http.server
import json
import os
import sys
import threading
import unittest

sys.path.insert(0, os.path.dirname(__file__))
from bridge_test_support import load_bridge  # noqa: E402


class RecordingHandler(http.server.BaseHTTPRequestHandler):
    """Answers every request like the app would, and keeps the headers it was sent."""

    received = []

    def _answer(self, body):
        payload = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        RecordingHandler.received.append(("GET", dict(self.headers)))
        self._answer({"name": "orca-slicer", "version": "test"})

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        request = json.loads(self.rfile.read(length))
        RecordingHandler.received.append(("POST", dict(self.headers)))
        self._answer({"jsonrpc": "2.0", "id": request.get("id"), "result": {"tools": []}})

    def log_message(self, *args):
        pass


def host_the_app_accepts(host, port):
    """The C++ rule's Host half, for the names the bridge can be configured with."""
    return host.lower() in {f"127.0.0.1:{port}", f"localhost:{port}", f"[::1]:{port}"}


class BridgeRequestHeadersTest(unittest.TestCase):
    def setUp(self):
        RecordingHandler.received = []
        self.server = http.server.HTTPServer(("127.0.0.1", 0), RecordingHandler)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.bridge = load_bridge()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()

    def point_bridge_at(self, host_name):
        self.bridge.ORCAMCP_URL = f"http://{host_name}:{self.port}/mcp"

    def headers_of(self, method):
        return [headers for m, headers in RecordingHandler.received if m == method]

    def test_a_forwarded_tool_call_sends_no_origin_and_names_this_machine(self):
        for host_name in ("localhost", "127.0.0.1"):
            with self.subTest(host=host_name):
                RecordingHandler.received = []
                self.point_bridge_at(host_name)
                reply = self.bridge.send_request({"jsonrpc": "2.0", "id": 7, "method": "tools/list", "params": {}})
                self.assertEqual(reply.get("id"), 7)
                (headers,) = self.headers_of("POST")
                lowered = {name.lower(): value for name, value in headers.items()}
                self.assertNotIn("origin", lowered)
                self.assertTrue(host_the_app_accepts(lowered["host"], self.port), lowered["host"])

    def test_the_liveness_probe_sends_no_origin_and_names_this_machine(self):
        self.point_bridge_at("localhost")
        self.assertEqual(self.bridge.check_orcaslicer_connection(use_cache=False), self.bridge.LIVE)
        (headers,) = self.headers_of("GET")
        lowered = {name.lower(): value for name, value in headers.items()}
        self.assertNotIn("origin", lowered)
        self.assertTrue(host_the_app_accepts(lowered["host"], self.port), lowered["host"])


if __name__ == "__main__":
    unittest.main()

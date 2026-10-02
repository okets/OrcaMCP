#!/usr/bin/env python3
"""The bridge answers no notification, and sends none to the app.

JSON-RPC never answers a notification (a method and no id). The bridge kept back only
notifications/initialized: any other -- notifications/cancelled, which a client sends when the user stops
a tool call -- went to the app, and the bridge printed the app's refusal as a reply with id 0, which no
request waits for. Since the app accepts a notification with 202 and no body (OrcaMCPTransport.hpp), that
reply would have been a parse error instead.

Run from the repo root:  python3 -m unittest discover -s scripts/tests -t scripts
"""

import contextlib
import io
import json
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bridge_test_support import load_bridge  # noqa: E402

NOTIFICATIONS = ("notifications/initialized", "notifications/cancelled", "notifications/roots/list_changed",
                 "notifications/not_yet_in_any_spec")


def run_bridge(bridge, messages):
    """What the bridge writes to its client for `messages`, one JSON-RPC message a line, in order."""
    stdin = io.StringIO("".join(json.dumps(message) + "\n" for message in messages))
    stdout = io.StringIO()
    with mock.patch.object(sys, "stdin", stdin), contextlib.redirect_stdout(stdout):
        bridge.main()
    return [json.loads(line) for line in stdout.getvalue().splitlines() if line.strip()]


class NotificationTests(unittest.TestCase):
    def setUp(self):
        self.bridge = load_bridge()

    def test_no_notification_is_answered_or_sent_to_the_app(self):
        for method in NOTIFICATIONS:
            with self.subTest(method=method):
                with mock.patch.object(self.bridge, "post_to_app") as post:
                    written = run_bridge(self.bridge, [
                        {"jsonrpc": "2.0", "method": method, "params": {"requestId": 3, "reason": "stopped"}},
                        {"jsonrpc": "2.0", "id": 5, "method": "ping"},
                    ])
                self.assertEqual(written, [{"jsonrpc": "2.0", "id": 5, "result": {}}])
                post.assert_not_called()

    def test_a_request_whose_id_is_null_is_still_answered(self):
        (written,) = run_bridge(self.bridge, [{"jsonrpc": "2.0", "id": None, "method": "ping"}])
        self.assertEqual(written["result"], {})

    def test_a_message_is_a_notification_only_with_a_method_and_no_id(self):
        self.assertTrue(self.bridge.is_notification({"jsonrpc": "2.0", "method": "notifications/cancelled"}))
        self.assertFalse(self.bridge.is_notification({"jsonrpc": "2.0", "id": None, "method": "ping"}))
        self.assertFalse(self.bridge.is_notification({"jsonrpc": "2.0", "id": 1, "method": "tools/list"}))
        self.assertFalse(self.bridge.is_notification({"jsonrpc": "2.0", "result": {}}))
        self.assertFalse(self.bridge.is_notification(["notifications/initialized"]))


if __name__ == "__main__":
    unittest.main()

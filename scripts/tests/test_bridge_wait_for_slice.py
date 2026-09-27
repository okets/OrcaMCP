"""wait_for_slice: the bridge polls get_slicing_status so the agent does not have to.

The tests point the bridge at a local server that plays the app: it answers get_slicing_status with
a scripted sequence of statuses, one per call, repeating the last. The poll interval is shortened so
a whole wait takes milliseconds.
"""

import contextlib
import http.server
import io
import json
import os
import socket
import sys
import threading
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(__file__))
from bridge_test_support import load_bridge  # noqa: E402


def status(is_slicing, state=None, outcome=None, message=None, plates=None):
    """A get_slicing_status answer, cut down to the fields wait_for_slice reads."""
    run = {"ended_early": outcome == "ended_early", "outcome": outcome}
    if message is not None:
        run["message"] = message
    return {
        "is_slicing": is_slicing,
        "state": state or ("slicing" if is_slicing else "idle"),
        "plates": plates or [{"index": 0, "slice_result_valid": state == "done", "percent": 100 if state == "done" else 40}],
        "slice_run": run,
    }


SLICING = status(True, outcome="running")
DONE = status(False, state="done", outcome="done")


class FakeApp(http.server.BaseHTTPRequestHandler):
    """Answers tools/call get_slicing_status with the next scripted status."""

    script = []
    calls = []
    delay_first_s = 0.0
    delay_every_s = 0.0
    rpc_error = None
    rpc_error_from_call = 1   # the first call that gets rpc_error
    drop_calls = set()        # calls answered by closing the connection, as an app that dies mid-reply
    on_answered = None        # called with the call number after each answer

    def do_GET(self):
        self._send({"name": "orca-slicer"})

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
        FakeApp.calls.append(request["params"]["name"])
        call = len(FakeApp.calls)
        if call == 1 and FakeApp.delay_first_s:
            time.sleep(FakeApp.delay_first_s)
        if FakeApp.delay_every_s:
            time.sleep(FakeApp.delay_every_s)
        if call in FakeApp.drop_calls:
            self.close_connection = True  # no reply at all: the bridge sees the connection close
            return
        if FakeApp.rpc_error is not None and call >= FakeApp.rpc_error_from_call:
            self._send({"jsonrpc": "2.0", "id": request.get("id"), "error": FakeApp.rpc_error})
            return
        answer = FakeApp.script[min(call, len(FakeApp.script)) - 1]
        self._send({"jsonrpc": "2.0", "id": request.get("id"),
                    "result": {"content": [{"type": "text", "text": json.dumps(answer)}]}})
        if FakeApp.on_answered is not None:
            FakeApp.on_answered(call)

    def _send(self, body):
        payload = json.dumps(body).encode()
        try:
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass  # the bridge gave up on a delayed answer, as it is meant to

    def log_message(self, *args):
        pass


class WaitForSliceTest(unittest.TestCase):
    def setUp(self):
        FakeApp.script, FakeApp.calls, FakeApp.delay_first_s, FakeApp.rpc_error = [DONE], [], 0.0, None
        FakeApp.delay_every_s, FakeApp.rpc_error_from_call, FakeApp.drop_calls, FakeApp.on_answered = 0.0, 1, set(), None
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FakeApp)
        self.server.daemon_threads = True
        threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True).start()
        self.bridge = load_bridge()
        self.bridge.ORCAMCP_URL = f"http://127.0.0.1:{self.server.server_address[1]}/mcp"
        self.bridge.WAIT_FOR_SLICE_POLL_S = 0.01

    def tearDown(self):
        self.stop_app()

    def stop_app(self):
        """Close the fake app's listening socket, so a connection is refused, as after a quit."""
        if self.server is not None:
            self.server.shutdown()
            self.server.server_close()
            self.server = None

    def call(self, arguments=None):
        """The tool call as a client makes it, and the result's decoded text."""
        request = {"jsonrpc": "2.0", "id": 9, "method": "tools/call",
                   "params": {"name": "wait_for_slice", "arguments": arguments or {}}}
        response = self.bridge.handle_local_request(request)
        result = response["result"]
        text = result["content"][0]["text"]
        return result, (text if result.get("isError") else json.loads(text))

    def test_the_bridge_answers_it_without_forwarding_the_call(self):
        self.call()
        self.assertEqual(FakeApp.calls, ["get_slicing_status"])

    def test_a_slice_in_progress_is_waited_out_and_reported_done(self):
        FakeApp.script = [SLICING, SLICING, DONE]
        result, report = self.call()
        self.assertFalse(result["isError"])
        self.assertEqual(report["outcome"], "done")
        self.assertFalse(report["timed_out"])
        self.assertEqual(report["polls"], 3)
        self.assertEqual(report["slicing_status"], DONE)

    def test_a_slice_still_running_at_the_timeout_is_reported_as_timed_out(self):
        FakeApp.script = [SLICING]
        started = time.monotonic()
        _, report = self.call({"timeout_s": 1})
        elapsed = time.monotonic() - started
        self.assertEqual(report["outcome"], "timed_out")
        self.assertTrue(report["timed_out"])
        self.assertEqual(report["slicing_status"], SLICING)
        self.assertGreaterEqual(elapsed, 1.0)
        self.assertLess(elapsed, 3.0)

    def test_the_cap_stays_fifteen_seconds_under_the_request_timeout(self):
        for request_timeout, cap in ((120, 105), (300, 285), (10, 5), (3, 5)):
            with self.subTest(ORCAMCP_TIMEOUT=request_timeout):
                self.bridge.TIMEOUT = request_timeout
                self.assertEqual(self.bridge.wait_for_slice_cap(), cap)

    def test_a_timeout_above_the_cap_is_cut_to_the_cap_and_says_so(self):
        self.bridge.TIMEOUT = 20
        _, report = self.call({"timeout_s": 500})
        self.assertEqual(report["timeout_s"], 5)
        self.assertEqual(report["timeout_cap_s"], 5)
        self.assertTrue(report["timeout_capped"])

    def test_a_timeout_within_the_cap_is_used_as_given(self):
        _, report = self.call({"timeout_s": 30})
        self.assertEqual(report["timeout_s"], 30)
        self.assertNotIn("timeout_capped", report)

    def test_without_a_timeout_the_wait_is_the_cap(self):
        _, report = self.call()
        self.assertEqual(report["timeout_s"], self.bridge.wait_for_slice_cap())

    def test_a_run_that_ended_early_ends_the_wait_with_that_outcome_and_its_reason(self):
        FakeApp.script = [SLICING, status(False, outcome="ended_early", message="Slice All stopped at plate 2")]
        _, report = self.call()
        self.assertEqual(report["outcome"], "ended_early")
        self.assertEqual(report["message"], "Slice All stopped at plate 2")
        self.assertFalse(report["timed_out"])

    def test_a_run_cancelled_by_a_plate_list_change_is_incomplete_not_done(self):
        FakeApp.script = [SLICING, status(False, state="done", outcome="incomplete",
                                          message="the plate list changed during the run")]
        _, report = self.call()
        self.assertEqual(report["outcome"], "incomplete")
        self.assertIn("plate list changed", report["message"])

    def test_nothing_slicing_and_no_result_returns_not_slicing_at_once(self):
        FakeApp.script = [status(False, state="idle")]
        _, report = self.call()
        self.assertEqual(report["outcome"], "not_slicing")
        self.assertEqual(report["polls"], 1)
        self.assertIn("slice_all", report["message"])

    def test_an_app_without_slice_run_outcomes_is_judged_by_the_selected_plate(self):
        FakeApp.script = [{"is_slicing": False, "state": "done", "plates": []}]
        _, report = self.call()
        self.assertEqual(report["outcome"], "done")

    def test_a_status_poll_the_app_is_too_busy_to_answer_does_not_end_the_wait(self):
        self.bridge.WAIT_FOR_SLICE_POLL_TIMEOUT_S = (0.05, 0.05)
        FakeApp.delay_first_s = 0.3
        FakeApp.script = [DONE]
        result, report = self.call({"timeout_s": 2})
        self.assertFalse(result["isError"])
        self.assertEqual(report["outcome"], "done")
        self.assertGreaterEqual(len(FakeApp.calls), 2)

    def test_an_app_that_is_not_running_gets_the_start_orca_advice(self):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            closed_port = probe.getsockname()[1]
        self.bridge.ORCAMCP_URL = f"http://127.0.0.1:{closed_port}/mcp"
        result, text = self.call()
        self.assertTrue(result["isError"])
        self.assertIn("start_orca", text)

    def test_an_error_from_the_app_ends_the_wait_with_it(self):
        FakeApp.rpc_error = {"code": -32603, "message": "Tool 'get_slicing_status' failed: boom"}
        result, text = self.call()
        self.assertTrue(result["isError"])
        self.assertIn("boom", text)

    def test_a_connection_dropped_mid_poll_does_not_end_the_wait(self):
        FakeApp.drop_calls = {1}
        FakeApp.script = [DONE]
        result, report = self.call({"timeout_s": 5})
        self.assertFalse(result["isError"])
        self.assertEqual(report["outcome"], "done")
        self.assertEqual(len(FakeApp.calls), 2)

    def test_an_app_that_quits_mid_wait_ends_it_as_app_gone(self):
        FakeApp.script = [SLICING]
        FakeApp.rpc_error = {"code": -32002, "message": "OrcaMCP is quitting: the call was not run"}
        FakeApp.rpc_error_from_call = 2
        result, report = self.call({"timeout_s": 5})
        self.assertFalse(result["isError"])
        self.assertEqual(report["outcome"], "app_gone")
        self.assertFalse(report["timed_out"])
        self.assertIn("start_orca", report["message"])
        self.assertEqual(report["slicing_status"], SLICING)

    def test_an_app_that_stops_listening_mid_wait_ends_it_as_app_gone(self):
        FakeApp.script = [SLICING]
        FakeApp.drop_calls = {2}  # the reply cut short as the app goes, then nothing listens
        FakeApp.on_answered = lambda call: threading.Thread(target=self.stop_app).start()
        result, report = self.call({"timeout_s": 5})
        self.assertFalse(result["isError"])
        self.assertEqual(report["outcome"], "app_gone")

    def test_a_request_the_bridge_fails_on_is_answered_under_its_own_id(self):
        line = json.dumps({"jsonrpc": "2.0", "id": 42, "method": "tools/call",
                           "params": {"name": "wait_for_slice", "arguments": {}}})
        out = io.StringIO()
        with mock.patch.object(self.bridge, "handle_local_request", side_effect=RuntimeError("unexpected")), \
                mock.patch.object(self.bridge.sys, "stdin", io.StringIO(line + "\n")), \
                contextlib.redirect_stdout(out):
            self.bridge.main()
        reply = json.loads(out.getvalue().splitlines()[0])
        self.assertEqual(reply["id"], 42)
        self.assertIn("unexpected", reply["error"]["message"])

    def test_a_timeout_that_is_not_a_number_of_seconds_is_refused_before_waiting(self):
        for bad in ("soon", 0, -3, 0.5, True, [5], float("nan")):
            with self.subTest(timeout_s=bad):
                FakeApp.calls = []
                result, text = self.call({"timeout_s": bad})
                self.assertTrue(result["isError"])
                self.assertIn("timeout_s", text)
                self.assertEqual(FakeApp.calls, [])

    def test_a_timeout_sent_as_a_numeric_string_is_accepted(self):
        _, report = self.call({"timeout_s": "12"})
        self.assertEqual(report["timeout_s"], 12)


if __name__ == "__main__":
    unittest.main()

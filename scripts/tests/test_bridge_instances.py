"""Several OrcaMCP instances at once: the bridge finds them, lets the agent choose one, and never sends a
call anywhere else.

Each test plays the instances with small local HTTP servers, and gives each one a registry entry in a
folder of its own, as the app writes them (OrcaMCPInstanceRegistry.hpp). No test reaches a real app,
and none launches one: bridge_test_support's bridge refuses to.

Run from the repo root:  python3 -m unittest discover -s scripts/tests -t scripts
"""

import datetime
import http.server
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bridge_test_support import closed_port, load_bridge, load_manifest  # noqa: E402

PROGRAM = "/Applications/OrcaMCP.app/Contents/MacOS/OrcaSlicer"
DATA_DIR = "/Users/someone/Library/Application Support/OrcaMCP"
TOOLS = [{"name": "get_scene_info", "description": "Scene.", "inputSchema": {"type": "object", "properties": {}}}]


def timestamp(seconds: float) -> str:
    """started_at as the app writes it: UTC, with milliseconds."""
    moment = datetime.datetime.fromtimestamp(seconds, datetime.timezone.utc)
    return moment.strftime("%Y-%m-%dT%H:%M:%S.") + f"{moment.microsecond // 1000:03d}Z"


class FakeInstance:
    """One OrcaMCP instance: GET /mcp tells who it is, and tools/call answers with its pid. Like the app,
    it refuses a call stamped for another instance with -32004 (unless it plays an older OrcaMCP)."""

    def __init__(self, pid, project="bracket", instance_id=None, legacy=False, tools=None, started_at=None,
                 executable=PROGRAM, data_dir=DATA_DIR, get_delay_s=0.0):
        self.calls = []
        self.legacy = legacy
        self.tools = tools or TOOLS
        self.get_delay_s = get_delay_s
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), self.handler())
        self.port = self.server.server_address[1]
        self.identity = {
            "schema": 1, "instance_id": instance_id or f"id-{pid}", "pid": pid, "port": self.port,
            "url": f"http://127.0.0.1:{self.port}/mcp", "version": "2.5.0.6-dev", "executable": executable,
            "data_dir": data_dir, "started_at": started_at or timestamp(time.time() - 60),
            "project": {"name": project, "path": f"/prints/{project}.3mf", "unsaved": False},
        }
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    @property
    def url(self):
        return self.identity["url"]

    def handler(self):
        instance = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                time.sleep(instance.get_delay_s)
                answer = {"name": "orca-slicer", "version": "2.5.0.5" if instance.legacy else "2.5.0.6-dev"}
                if not instance.legacy:
                    answer["instance"] = instance.identity
                self.answer(answer)

            def do_POST(self):
                request = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
                instance.calls.append(request)
                if request["method"] == "tools/list":
                    return self.answer({"jsonrpc": "2.0", "id": request["id"], "result": {"tools": instance.tools}})
                stamp = (request["params"].get("_meta") or {}).get("orcamcp/instance")
                if not instance.legacy and stamp is not None and stamp != instance.identity["instance_id"]:
                    return self.answer({"jsonrpc": "2.0", "id": request["id"],
                                        "error": {"code": -32004, "message": "meant for another instance"}})
                text = json.dumps({"status": "success", "answered_by": instance.identity["pid"]})
                self.answer({"jsonrpc": "2.0", "id": request["id"], "result": {"content": [{"type": "text", "text": text}]}})

            def answer(self, body):
                data = json.dumps(body).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *args):
                pass

        return Handler

    def stop(self):
        self.server.shutdown()
        self.server.server_close()


def dead_pid() -> int:
    """The pid of a process that has finished."""
    process = subprocess.Popen([sys.executable, "-c", "pass"])
    process.wait()
    return process.pid


class InstancesTest(unittest.TestCase):
    """A bridge with its own registry folder, and helpers to add instances and call tools as main() does."""

    def setUp(self):
        self.bridge = load_bridge()
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.bridge.INSTANCES_DIR = self.folder.name
        self.instances = []
        self.steps = []  # every next_steps the bridge answered with, checked in tearDown

    def tearDown(self):
        for instance in self.instances:
            instance.stop()
        self.check_next_steps_name_real_tools()

    def start(self, pid, registered=True, **kwargs) -> FakeInstance:
        instance = FakeInstance(pid, **kwargs)
        self.instances.append(instance)
        if registered:
            self.register(instance.identity)
        return instance

    def register(self, entry: dict):
        with open(os.path.join(self.folder.name, f"{entry['pid']}.json"), "w", encoding="utf-8") as f:
            json.dump(entry, f)

    def call(self, name, arguments=None) -> dict:
        """A tools/call through the bridge, as main() handles it; the answer's JSON, and whether it is an error."""
        request = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                   "params": {"name": name, "arguments": arguments or {}}}
        response = self.bridge.handle_local_request(request)
        if response is None:
            response = self.bridge.settle_call(request, self.bridge.send_request(request))
        if "error" in response:
            return {"rpc_error": response["error"]}
        result = response["result"]
        text = result["content"][0]["text"]
        try:
            report = json.loads(text)
        except ValueError:
            report = {"text": text}
        report["is_error"] = result.get("isError", False)
        self.steps.extend(report.get("next_steps", []) if isinstance(report, dict) else [])
        return report

    def check_next_steps_name_real_tools(self):
        """Every next step the bridge gave names a real tool, with arguments that tool takes."""
        manifest = load_manifest()
        tools = {t["name"]: t for t in manifest["server_tools"] + manifest["bridge_tools"]}
        for step in self.steps:
            with self.subTest(step=step):
                self.assertIn(step["tool"], tools)
                self.assertTrue(step["why"])
                self.assertIsNone(self.bridge.argument_error(step["tool"], tools[step["tool"]]["inputSchema"],
                                                             step.get("arguments", {})))

    def answered_by(self, report):
        return report.get("answered_by")


class FindingInstancesTests(InstancesTest):
    def test_every_running_instance_is_listed_with_its_open_project(self):
        first = self.start(40001, project="benchy")
        second = self.start(40002, project="bracket")
        report = self.call("list_instances")
        by_port = sorted([first, second], key=lambda instance: instance.port)
        self.assertEqual([i["pid"] for i in report["instances"]], [i.identity["pid"] for i in by_port])
        listed = {i["pid"]: i for i in report["instances"]}
        self.assertEqual(listed[40001]["project"], {"name": "benchy", "path": "/prints/benchy.3mf", "unsaved": False})
        self.assertEqual(listed[40002]["port"], second.port)
        self.assertEqual(listed[40002]["executable"], PROGRAM)
        self.assertEqual(listed[40002]["data_dir"], DATA_DIR)
        self.assertEqual(listed[40001]["state"], "live")

    def test_an_entry_left_by_a_crashed_instance_is_ignored(self):
        self.start(40001)
        self.register({"schema": 1, "instance_id": "id-crashed", "pid": dead_pid(), "port": closed_port(),
                       "url": f"http://127.0.0.1:{closed_port()}/mcp"})
        report = self.call("list_instances")
        self.assertEqual([i["pid"] for i in report["instances"]], [40001])
        self.assertEqual(report["stale_entries_ignored"], 1)

    def test_an_entry_whose_port_another_instance_answers_is_stale(self):
        other = self.start(40002)
        self.register(dict(other.identity, instance_id="id-gone", pid=40001))  # its port now answers as 40002
        report = self.call("list_instances")
        self.assertEqual([i["pid"] for i in report["instances"]], [40002])
        self.assertEqual(report["stale_entries_ignored"], 1)

    def test_a_busy_instance_whose_process_runs_is_listed_as_busy(self):
        busy = self.start(os.getpid(), get_delay_s=1.0)  # its one request thread is in a long call
        report = self.call("list_instances")
        self.assertEqual([(i["port"], i["state"]) for i in report["instances"]], [(busy.port, "busy")])

    def test_a_file_that_is_not_an_entry_is_skipped(self):
        self.start(40001)
        with open(os.path.join(self.folder.name, "999.json"), "w") as f:
            f.write('{"pid": 999')
        self.assertEqual([i["pid"] for i in self.call("list_instances")["instances"]], [40001])

    def test_an_older_orcamcp_at_the_first_port_is_listed_as_legacy(self):
        older = self.start(0, registered=False, legacy=True)
        self.bridge.ORCAMCP_URL = older.url
        report = self.call("list_instances")
        self.assertEqual(len(report["instances"]), 1)
        self.assertTrue(report["instances"][0]["legacy"])
        self.assertEqual(report["instances"][0]["port"], older.port)
        self.assertNotIn("pid", report["instances"][0])

    def test_instances_on_one_data_folder_are_marked(self):
        self.start(40001)
        self.start(40002)
        self.start(40003, data_dir="/copies/OrcaMCP")
        listed = {i["pid"]: i for i in self.call("list_instances")["instances"]}
        self.assertEqual(listed[40001]["shares_data_dir_with"], [40002])
        self.assertEqual(listed[40002]["shares_data_dir_with"], [40001])
        self.assertNotIn("shares_data_dir_with", listed[40003])

    def test_with_nothing_running_the_list_says_to_start_orcamcp(self):
        report = self.call("list_instances")
        self.assertEqual(report["instances"], [])
        self.assertEqual([s["tool"] for s in report["next_steps"]], ["start_orca"])


class ChoosingTests(InstancesTest):
    def test_with_one_instance_nothing_changes(self):
        only = self.start(40001)
        report = self.call("get_scene_info")
        self.assertEqual(self.answered_by(report), 40001)
        stamp = only.calls[-1]["params"]["_meta"]["orcamcp/instance"]
        self.assertEqual(stamp, "id-40001")
        self.assertTrue(self.call("list_instances")["instances"][0]["selected"])

    def test_with_several_and_none_chosen_a_call_is_refused_with_the_list(self):
        first = self.start(40001)
        second = self.start(40002)
        report = self.call("new_project")
        self.assertTrue(report["is_error"])
        self.assertIn("not run", report["message"])
        self.assertEqual({i["pid"] for i in report["instances"]}, {40001, 40002})
        self.assertEqual(report["next_steps"][0]["tool"], "select_instance")
        self.assertEqual(first.calls + second.calls, [])  # neither instance saw it

    def test_orcamcp_port_starts_with_the_instance_on_that_port(self):
        self.start(40001)
        pinned = self.start(40002)
        self.bridge.PINNED_PORT = pinned.port
        self.assertEqual(self.answered_by(self.call("get_scene_info")), 40002)

    def test_with_nothing_running_a_call_says_to_start_orcamcp(self):
        report = self.call("get_scene_info")
        self.assertTrue(report["is_error"])
        self.assertIn("not running", report["text"])

    def test_select_instance_by_pid_port_project_name_or_path(self):
        first = self.start(40001, project="benchy")
        second = self.start(40002, project="Bracket")
        for arguments, pid in (({"pid": 40002}, 40002), ({"port": first.port}, 40001),
                               ({"project": "bracket"}, 40002), ({"project": "/prints/benchy.3mf"}, 40001)):
            with self.subTest(arguments=arguments):
                report = self.call("select_instance", arguments)
                self.assertEqual(report["status"], "success")
                self.assertEqual(report["instance"]["pid"], pid)
                self.assertTrue(report["instance"]["selected"])
                self.assertEqual(self.answered_by(self.call("get_scene_info")), pid)
        self.assertEqual(second.calls[-1]["params"]["_meta"]["orcamcp/instance"], "id-40002")

    def test_select_instance_says_which_it_used_before(self):
        self.start(40001)
        self.start(40002)
        self.call("select_instance", {"pid": 40001})
        report = self.call("select_instance", {"pid": 40002})
        self.assertEqual(report["previous"]["pid"], 40001)

    def test_select_instance_refuses_a_project_several_instances_have_open(self):
        self.start(40001, project="bracket")
        self.start(40002, project="bracket")
        report = self.call("select_instance", {"project": "bracket"})
        self.assertTrue(report["is_error"])
        self.assertIn("2 running instances", report["message"])
        self.assertIsNone(self.bridge._selected)

    def test_select_instance_refuses_what_no_instance_has(self):
        self.start(40001)
        report = self.call("select_instance", {"pid": 99999})
        self.assertTrue(report["is_error"])
        self.assertIsNone(self.bridge._selected)

    def test_select_instance_takes_exactly_one_of_pid_port_or_project(self):
        self.start(40001)
        for arguments in ({}, {"pid": 40001, "port": 13618}, {"pid": "40001"}, {"project": ""}, {"port": True}):
            with self.subTest(arguments=arguments):
                self.assertEqual(self.call("select_instance", arguments)["rpc_error"]["code"], -32602)

    def test_the_suggested_choice_is_an_instance_that_tells_who_it_is(self):
        older = self.start(0, registered=False, legacy=True)
        self.start(40002)
        self.bridge.ORCAMCP_URL = older.url
        report = self.call("list_instances")
        self.assertEqual(report["next_steps"][0], {"tool": "select_instance", "arguments": {"pid": 40002},
                                                   "why": "several instances run and this session has not chosen one"})

    def test_an_older_orcamcp_is_never_suggested(self):
        older = self.start(0, registered=False, legacy=True)
        chosen = self.start(40001)
        self.bridge.ORCAMCP_URL = older.url
        self.call("select_instance", {"pid": 40001})
        chosen.stop()
        self.instances.remove(chosen)
        report = self.call("get_scene_info")
        self.assertNotIn("select_instance", [step["tool"] for step in report["next_steps"]])
        self.assertEqual([i["port"] for i in report["instances"]], [older.port])

    def test_an_older_orcamcp_can_be_chosen_by_its_port(self):
        older = self.start(0, registered=False, legacy=True)
        self.start(40002)
        self.bridge.ORCAMCP_URL = older.url
        report = self.call("select_instance", {"port": older.port})
        self.assertTrue(report["instance"]["legacy"])
        self.call("get_scene_info")
        self.assertEqual(older.calls[-1]["params"]["_meta"]["orcamcp/instance"], "legacy")


class LosingTheInstanceTests(InstancesTest):
    def test_a_quit_instance_is_never_replaced_by_another_silently(self):
        chosen = self.start(40001)
        other = self.start(40002, started_at=timestamp(time.time() - 3600))  # ran beside it all along
        self.call("select_instance", {"pid": 40001})
        chosen.stop()
        self.instances.remove(chosen)
        report = self.call("new_project")
        self.assertTrue(report["is_error"])
        self.assertIn("not run", report["message"])
        self.assertIn("pid 40001", report["message"])
        self.assertEqual([i["pid"] for i in report["instances"]], [40002])
        self.assertEqual(other.calls, [])
        self.assertEqual(self.bridge._selected["instance_id"], "id-40001")  # still the agent's choice
        self.assertEqual(self.call("list_instances")["using"]["state"], "gone")

    def test_a_restart_of_the_chosen_instance_is_followed_and_the_call_is_not_run(self):
        chosen = self.start(40001)
        self.call("select_instance", {"pid": 40001})
        chosen.stop()
        self.instances.remove(chosen)
        restarted = self.start(40009, started_at=timestamp(time.time() + 1))
        report = self.call("delete_object", {"object_id": 0})
        self.assertTrue(report["is_error"])
        self.assertIn("restarted", report["message"])
        self.assertEqual([c for c in restarted.calls if c["method"] == "tools/call"], [])  # the call was not run
        self.assertEqual(report["next_steps"][0]["tool"], "get_scene_info")
        self.assertEqual(self.answered_by(self.call("get_scene_info")), 40009)

    def test_a_restart_on_another_program_or_data_folder_is_not_followed(self):
        for different in ({"executable": "/tmp/build/OrcaSlicer"}, {"data_dir": "/copies/OrcaMCP"}):
            with self.subTest(different=different):
                self.bridge._selected = None
                chosen = self.start(40001)
                self.call("select_instance", {"pid": 40001})
                chosen.stop()
                self.instances.remove(chosen)
                newcomer = self.start(40010, started_at=timestamp(time.time() + 1), **different)
                report = self.call("get_scene_info")
                self.assertIn("no other instance is chosen", report["message"])
                self.assertEqual(newcomer.calls, [])
                newcomer.stop()
                self.instances.remove(newcomer)
                for name in os.listdir(self.folder.name):
                    os.remove(os.path.join(self.folder.name, name))

    def test_a_call_that_reaches_another_instance_on_the_chosen_port_is_not_run(self):
        chosen = self.start(40001)
        self.call("select_instance", {"pid": 40001})
        # The instance quit, and another took its port: it answers GET as itself and refuses the stamp.
        chosen.identity = dict(chosen.identity, instance_id="id-40011", pid=40011, started_at=timestamp(time.time() - 3600))
        os.remove(os.path.join(self.folder.name, "40001.json"))
        self.register(chosen.identity)
        report = self.call("new_project")
        self.assertTrue(report["is_error"])
        self.assertIn("another instance does", report["message"])
        self.assertIn("not run", report["message"])
        refused = [c for c in chosen.calls if c["method"] == "tools/call"]
        self.assertEqual(len(refused), 1)  # the one it refused with -32004, unrun

    def test_with_nothing_left_running_the_answer_says_to_start_orcamcp(self):
        chosen = self.start(40001)
        self.call("select_instance", {"pid": 40001})
        chosen.stop()
        self.instances.remove(chosen)
        report = self.call("get_scene_info")
        self.assertIn("No OrcaMCP is running", report["message"])
        self.assertEqual([s["tool"] for s in report["next_steps"]], ["start_orca"])


class ToolListTests(InstancesTest):
    def test_switching_between_instances_of_one_build_keeps_the_tool_list(self):
        self.start(40001)
        self.start(40002)
        self.bridge.served(self.bridge.with_bridge_tools(TOOLS))
        self.call("select_instance", {"pid": 40001})
        report = self.call("select_instance", {"pid": 40002})
        self.assertFalse(report["tool_list_changed"])
        self.assertFalse(self.bridge.tools_changed_notification_due())

    def test_switching_to_another_build_tells_the_client_its_tool_list_changed(self):
        self.start(40001)
        self.start(40002, tools=TOOLS + [{"name": "repair_mesh", "description": "Repair.",
                                          "inputSchema": {"type": "object", "properties": {}}}])
        self.bridge.served(self.bridge.with_bridge_tools(TOOLS))
        self.call("select_instance", {"pid": 40001})
        report = self.call("select_instance", {"pid": 40002})
        self.assertTrue(report["tool_list_changed"])
        self.assertTrue(self.bridge.tools_changed_notification_due())
        self.assertFalse(self.bridge.tools_changed_notification_due())  # once


class StartOrcaTests(InstancesTest):
    def launches_into(self, instance_args):
        """A fake launch that starts an instance, as the app would, and records the program."""
        launched = []

        def launch(executable):
            launched.append(executable)
            self.start(40050, started_at=timestamp(time.time()), **instance_args)
        return launched, launch

    def start_orca(self, **arguments):
        with mock.patch.object(self.bridge, "get_orcamcp_executable", return_value=PROGRAM), \
                mock.patch.object(self.bridge.time, "sleep"):
            return self.call("start_orca", arguments)

    def test_an_instance_running_is_named_and_nothing_is_launched(self):
        self.start(40001)
        report = self.start_orca()
        self.assertEqual(report["status"], "already_running")
        self.assertEqual(report["instance"]["pid"], 40001)
        self.assertEqual(report["instance"]["executable"], PROGRAM)
        self.assertEqual(report["instance"]["data_dir"], DATA_DIR)
        self.assertIn("program", report["message"])
        self.assertIn("data folder", report["message"])

    def test_several_running_and_none_chosen_launches_nothing_and_lists_them(self):
        self.start(40001)
        self.start(40002)
        report = self.start_orca()
        self.assertTrue(report["is_error"])
        self.assertEqual(report["status"], "several_running")
        self.assertEqual(len(report["other_instances"]), 2)
        self.assertEqual(report["next_steps"][0]["tool"], "select_instance")

    def test_with_none_running_it_launches_the_app_and_uses_the_new_instance(self):
        launched, launch = self.launches_into({})
        with mock.patch.object(self.bridge, "launch_process", side_effect=launch):
            report = self.start_orca()
        self.assertEqual(launched, [PROGRAM])
        self.assertEqual(report["status"], "started")
        self.assertEqual(report["instance"]["pid"], 40050)
        self.assertEqual(self.bridge._selected["pid"], 40050)

    def test_new_instance_launches_another_even_with_one_running(self):
        self.start(40001)
        self.call("select_instance", {"pid": 40001})
        launched, launch = self.launches_into({})
        with mock.patch.object(self.bridge, "launch_process", side_effect=launch):
            report = self.start_orca(new_instance=True)
        self.assertEqual(report["status"], "started")
        self.assertEqual(report["instance"]["pid"], 40050)
        self.assertEqual([i["pid"] for i in report["other_instances"]], [40001])

    def test_when_the_chosen_instance_is_gone_it_launches_one(self):
        chosen = self.start(40001)
        self.start(40002, started_at=timestamp(time.time() - 3600))
        self.call("select_instance", {"pid": 40001})
        chosen.stop()
        self.instances.remove(chosen)
        launched, launch = self.launches_into({})
        with mock.patch.object(self.bridge, "launch_process", side_effect=launch):
            report = self.start_orca()
        self.assertEqual(report["status"], "started")
        self.assertEqual(self.bridge._selected["pid"], 40050)

    def test_new_instance_must_be_true_or_false(self):
        self.assertEqual(self.call("start_orca", {"new_instance": "yes"})["rpc_error"]["code"], -32602)


class ProcessCheckTests(unittest.TestCase):
    def test_a_running_process_is_alive_and_a_finished_one_is_not(self):
        bridge = load_bridge()
        self.assertTrue(bridge.pid_alive(os.getpid()))
        self.assertFalse(bridge.pid_alive(dead_pid()))
        self.assertFalse(bridge.pid_alive(0))

    def test_on_windows_no_signal_is_sent(self):
        """os.kill(pid, 0) sends CTRL_C_EVENT there."""
        bridge = load_bridge()
        kernel32 = mock.Mock()
        kernel32.OpenProcess.return_value = 0
        kernel32.GetLastError.return_value = 87  # ERROR_INVALID_PARAMETER: no such process
        fake_ctypes = mock.Mock(windll=mock.Mock(kernel32=kernel32))
        with mock.patch.object(bridge.os, "name", "nt"), mock.patch.dict(sys.modules, {"ctypes": fake_ctypes}), \
                mock.patch.object(bridge.os, "kill", side_effect=AssertionError("os.kill on Windows")):
            self.assertFalse(bridge.pid_alive(4242))
        kernel32.OpenProcess.assert_called_once()


class RestartRuleTests(unittest.TestCase):
    def setUp(self):
        self.bridge = load_bridge()
        self.previous = {"instance_id": "a", "pid": 1, "port": 13618, "url": "", "executable": PROGRAM,
                         "data_dir": DATA_DIR, "started_at": timestamp(1000)}

    def candidate(self, started, **changes):
        return dict(self.previous, instance_id="b", pid=2, started_at=timestamp(started), **changes)

    def test_the_same_program_on_the_same_data_folder_started_after_it_was_last_heard_from_is_a_restart(self):
        self.assertTrue(self.bridge.is_successor(self.candidate(2000.5), self.previous, 2000.0))

    def test_one_started_before_it_was_last_heard_from_ran_beside_it(self):
        self.assertFalse(self.bridge.is_successor(self.candidate(1999.5), self.previous, 2000.0))

    def test_another_program_or_data_folder_is_not_a_restart(self):
        self.assertFalse(self.bridge.is_successor(self.candidate(2001, executable="/other"), self.previous, 2000.0))
        self.assertFalse(self.bridge.is_successor(self.candidate(2001, data_dir="/other"), self.previous, 2000.0))

    def test_an_older_orcamcp_is_never_a_restart(self):
        self.assertFalse(self.bridge.is_successor({"legacy": True, "port": 13618}, self.previous, 0))


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""
OrcaMCP Bridge

Translates MCP stdio protocol to HTTP requests to OrcaSlicer's embedded HTTP server.
This allows Claude Code to communicate with OrcaSlicer via the standard MCP interface.

The bridge operates in two modes:
- Connected: Forwards requests to OrcaSlicer
- Disconnected: Returns cached tools list and helpful error messages

This ensures the MCP server stays healthy even when OrcaSlicer isn't running,
avoiding alarming error alerts in MCP clients.

Usage:
    python3 orcamcp-bridge.py

Configuration (Claude Code):
    Add to ~/.claude.json or project .mcp.json:
    {
        "mcpServers": {
            "orca-slicer": {
                "command": "python3",
                "args": ["/path/to/OrcaMCP/scripts/orcamcp-bridge.py"]
            }
        }
    }
"""

import sys
import http.client
import json
import math
import os
import socket
import subprocess
import time
import urllib.request
import urllib.error

# Every tool's name, description and schema, generated from the app's tool registry and checked
# against it by tests/slic3rutils/test_mcp_tool_list.cpp. The bridge serves its server_tools while
# the app is not running, and takes its own tools (bridge_tools, e.g. start_orca) from it whether
# the app runs or not, so a client sees the same names and descriptions before and after the app
# starts. None of that text is written here; the one exception is the start_orca offered when the
# file itself is unusable (fallback_bridge_tools).
TOOLS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "orcamcp_tools.json")


def list_entry(tool: dict) -> dict:
    """A tool as tools/list serves it. The manifest also carries each tool's category and summary."""
    return {"name": tool["name"], "description": tool["description"], "inputSchema": tool["inputSchema"]}


def _list_entries(manifest, key: str) -> list:
    """The manifest's `key` array as tools/list entries. Raises ValueError naming a malformed entry."""
    tools = manifest.get(key) if isinstance(manifest, dict) else None
    if not isinstance(tools, list):
        raise ValueError(f"it has no {key} list")
    for i, tool in enumerate(tools):
        if not (isinstance(tool, dict) and isinstance(tool.get("name"), str)
                and isinstance(tool.get("description"), str) and isinstance(tool.get("inputSchema"), dict)):
            raise ValueError(f"{key}[{i}] lacks a string name, a string description or an object inputSchema")
    return [list_entry(tool) for tool in tools]


def fallback_bridge_tools(error: str) -> list:
    """What the bridge offers when orcamcp_tools.json is unusable: start_orca alone, since without it
    an agent could not even launch the app.

    Its description deliberately differs from the file's start_orca -- it has to say what is wrong --
    so in this error state the offline and online lists do not match. They could not anyway: the
    offline list has lost every app tool.
    """
    return [{
        "name": "start_orca",
        "description": ("Launch OrcaMCP and wait until it is ready. The bridge could not read its tool list "
                        f"({error}), so this is the only tool it can offer while the app is down. Reinstall "
                        "OrcaMCP, or reconnect your agent from the app, to restore the rest."),
        "inputSchema": {"type": "object", "properties": {}, "required": [], "additionalProperties": False},
    }]


def load_tools_manifest(path: str = TOOLS_FILE) -> tuple:
    """Return (server_tools, bridge_tools, error), as tools/list entries.

    Never raises: a bridge that dies lists nothing at all. An unreadable or malformed file gives no
    app tools, the fallback start_orca, and the reason.
    """
    try:
        with open(path, encoding="utf-8") as f:
            manifest = json.load(f)
        return _list_entries(manifest, "server_tools"), _list_entries(manifest, "bridge_tools"), None
    except Exception as e:
        error = f"cannot read OrcaMCP's tool list {path}: {e}"
        return [], fallback_bridge_tools(error), error


def install_tools_manifest(path: str = TOOLS_FILE):
    """Read the tool list the bridge serves. Runs once at import; tests point it at other files."""
    global OFFLINE_SERVER_TOOLS, BRIDGE_TOOLS, TOOLS_MANIFEST_ERROR
    OFFLINE_SERVER_TOOLS, BRIDGE_TOOLS, TOOLS_MANIFEST_ERROR = load_tools_manifest(path)
    if TOOLS_MANIFEST_ERROR:
        print(f"[orcamcp-bridge] {TOOLS_MANIFEST_ERROR}", file=sys.stderr)


install_tools_manifest()

# Configuration
ORCAMCP_HOST = os.environ.get("ORCAMCP_HOST", "localhost")
ORCAMCP_PORT = int(os.environ.get("ORCAMCP_PORT", "13618"))
ORCAMCP_URL = f"http://{ORCAMCP_HOST}:{ORCAMCP_PORT}/mcp"
TIMEOUT = int(os.environ.get("ORCAMCP_TIMEOUT", "120"))  # 2 minute default for slicing

# What a tool call gets while nothing answers at ORCAMCP_URL.
NOT_RUNNING_MESSAGE = "OrcaMCP is not running. Use the 'start_orca' tool to start it, then try again."

# Server info for when OrcaMCP isn't connected
# Version is omitted since we don't know the actual OrcaMCP version
SERVER_INFO = {
    "name": "orca-slicer",
    "version": "unknown (OrcaMCP not running)",
    "protocolVersion": "2024-11-05"
}

# Cached tools list - served when OrcaSlicer isn't available
# This allows MCP clients to see available tools even when OrcaSlicer is offline
CACHED_TOOLS = None  # Will be populated on first successful connection

# Connection state cache to avoid repeated slow checks during startup
_connection_cache = {"connected": None, "last_check": 0}
CONNECTION_CACHE_TTL = 3  # seconds

# The bridge answers the first tools/list from orcamcp_tools.json when OrcaSlicer is not up yet --
# the normal case, since the MCP client starts first. That file is checked against the registry it
# ships with, but a running app from another build can still differ from it, and whatever the first
# list said is then the client's tool list for the session. These three flags let the bridge say
# "the list changed" the moment a live server is first reached.
_served_static_tools = False
_live_contact = False
_notified_tools_changed = False


def get_orcamcp_executable() -> str | None:
    """Find the OrcaMCP executable based on platform"""
    import platform
    system = platform.system()

    # Get project root (scripts/ is one level down from project root)
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(script_dir)

    paths = []

    # First priority: environment variable
    custom_path = os.environ.get("ORCAMCP_APP_PATH")
    if custom_path:
        paths.append(custom_path)

    if system == "Darwin":  # macOS
        # Dev build paths (relative to project root)
        paths.extend([
            os.path.join(project_root, "build", "arm64", "src", "Release", "OrcaSlicer.app", "Contents", "MacOS", "OrcaSlicer"),
            os.path.join(project_root, "build", "x86_64", "src", "Release", "OrcaSlicer.app", "Contents", "MacOS", "OrcaSlicer"),
            os.path.join(project_root, "build", "src", "Release", "OrcaSlicer.app", "Contents", "MacOS", "OrcaSlicer"),
        ])
        # Installation paths
        paths.extend([
            "/Applications/OrcaMCP.app/Contents/MacOS/OrcaSlicer",
            os.path.expanduser("~/Applications/OrcaMCP.app/Contents/MacOS/OrcaSlicer"),
        ])
    elif system == "Windows":
        # Dev build paths (relative to project root)
        paths.extend([
            os.path.join(project_root, "build", "OrcaSlicer", "orca-mcp.exe"),
            os.path.join(project_root, "build", "src", "Release", "orca-mcp.exe"),
            os.path.join(project_root, "build", "Release", "orca-mcp.exe"),
        ])
        # Installation paths
        paths.extend([
            os.path.join(os.environ.get("ProgramFiles", "C:\\Program Files"), "OrcaMCP", "orca-mcp.exe"),
            os.path.join(os.environ.get("ProgramFiles(x86)", "C:\\Program Files (x86)"), "OrcaMCP", "orca-mcp.exe"),
            os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs", "OrcaMCP", "orca-mcp.exe"),
        ])
    else:  # Linux
        # Dev build paths (relative to project root)
        paths.extend([
            os.path.join(project_root, "build", "src", "orca-mcp"),
            os.path.join(project_root, "build", "OrcaSlicer", "orca-mcp"),
        ])
        # Installation paths
        paths.extend([
            "/usr/bin/orcamcp",
            "/usr/local/bin/orcamcp",
            os.path.expanduser("~/.local/bin/orcamcp"),
            "/opt/OrcaMCP/bin/orcamcp",
        ])

    for path in paths:
        if path and os.path.isfile(path):
            log_debug(f"Found OrcaMCP executable: {path}")
            return path

    log_debug(f"OrcaMCP executable not found. Searched paths: {paths}")
    return None


def launch_orcamcp() -> dict:
    """Launch OrcaMCP application and wait for it to be ready"""
    # Check if already running
    if check_orcaslicer_connection() != DOWN:
        return {"success": True, "message": "OrcaMCP is already running"}

    executable = get_orcamcp_executable()
    if not executable:
        # Get project root for helpful message
        script_dir = os.path.dirname(os.path.abspath(__file__))
        project_root = os.path.dirname(script_dir)
        return {
            "success": False,
            "message": f"Could not find OrcaMCP executable. "
                      f"Searched in dev build paths (relative to {project_root}) and standard installation locations. "
                      f"Set ORCAMCP_APP_PATH environment variable to specify the path, or build the project first."
        }

    try:
        log_debug(f"Launching OrcaMCP from: {executable}")

        # An agent-launched app skips the Orca cloud silent sign-in: it reads the keychain
        # synchronously at startup, which on macOS can block on a permission prompt before the MCP
        # server exists (see CLAUDE.md, environment variables).
        agent_env = dict(os.environ, ORCAMCP_SKIP_CLOUD_LOGIN="1")

        # Launch detached from this process
        import platform
        if platform.system() == "Windows":
            # Windows: use CREATE_NEW_PROCESS_GROUP and DETACHED_PROCESS
            DETACHED_PROCESS = 0x00000008
            CREATE_NEW_PROCESS_GROUP = 0x00000200
            subprocess.Popen(
                [executable],
                creationflags=DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                close_fds=True,
                env=agent_env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL
            )
        elif platform.system() == "Darwin":
            # macOS: use 'open' command for .app bundles
            app_path = executable.replace("/Contents/MacOS/OrcaSlicer", "")
            if app_path.endswith(".app"):
                # --env reaches the app through LaunchServices; a plain env= would not.
                subprocess.Popen(["open", "--env", "ORCAMCP_SKIP_CLOUD_LOGIN=1", app_path], close_fds=True)
            else:
                subprocess.Popen(
                    [executable],
                    start_new_session=True,
                    close_fds=True,
                    env=agent_env,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL
                )
        else:
            # Linux: start new session
            subprocess.Popen(
                [executable],
                start_new_session=True,
                env=agent_env,
                close_fds=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL
            )

        # Wait for the HTTP server to become available
        max_wait = 30  # seconds
        start_time = time.time()

        while time.time() - start_time < max_wait:
            time.sleep(1)
            # Bypass cache when polling for startup
            if check_orcaslicer_connection(use_cache=False) == LIVE:
                elapsed = int(time.time() - start_time)
                return {
                    "success": True,
                    "message": f"OrcaMCP started successfully (took {elapsed}s)"
                }

        return {
            "success": False,
            "message": f"OrcaMCP was launched but HTTP server did not respond within {max_wait}s. "
                      "The application may still be starting up."
        }

    except Exception as e:
        return {
            "success": False,
            "message": f"Failed to launch OrcaMCP: {str(e)}"
        }


def log_debug(message: str):
    """Log debug message to stderr (won't interfere with stdout protocol)"""
    if os.environ.get("ORCAMCP_DEBUG"):
        print(f"[orcamcp-bridge] {message}", file=sys.stderr)


def make_error_response(request_id, code: int, message: str) -> dict:
    """Create a properly formatted JSON-RPC error response"""
    return {
        "jsonrpc": "2.0",
        "id": request_id if request_id is not None else 0,
        "error": {
            "code": code,
            "message": message
        }
    }


def make_success_response(request_id, result: dict) -> dict:
    """Create a properly formatted JSON-RPC success response"""
    return {
        "jsonrpc": "2.0",
        "id": request_id if request_id is not None else 0,
        "result": result
    }


def make_tool_error_result(message: str) -> dict:
    """Create an MCP tool result indicating an error (not a JSON-RPC error)"""
    return {
        "content": [
            {
                "type": "text",
                "text": message
            }
        ],
        "isError": True
    }


def note_live_contact():
    """Record that a live OrcaSlicer answered. Cheap enough to call on every success."""
    global _live_contact
    _live_contact = True


def tools_changed_notification_due() -> bool:
    """True at most once: a static tool list was served, and a live server has since answered."""
    global _notified_tools_changed
    if _notified_tools_changed or not _served_static_tools or not _live_contact:
        return False
    _notified_tools_changed = True
    return True


# One literal, used at both call sites in main(), so a typo in the method name or a dropped
# notification can only happen once, not independently in two copies.
TOOLS_CHANGED_NOTIFICATION = json.dumps({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})


def emit_tools_changed_notification():
    """Print notifications/tools/list_changed if one is due; a no-op otherwise."""
    if not tools_changed_notification_due():
        return
    print(TOOLS_CHANGED_NOTIFICATION, flush=True)
    log_debug("Told the client its tool list is stale")


# What a liveness probe concluded. Three values, not two, because the advice differs completely:
# a refused connection means "start the app", a timeout means "it is busy, wait or raise the
# timeout", and telling a user to restart a running application is the worse of the two mistakes.
LIVE = "live"  # something answered, even an HTTP error
BUSY = "busy"  # reachable but did not answer in time, or failed in a way that is not proof of death
DOWN = "down"  # connection refused, no listener, or the host does not resolve


def _verdict_for_exception(exc) -> str:
    """Classify a urlopen failure. Unknown failures are BUSY: absence of an answer is not proof."""
    if isinstance(exc, urllib.error.HTTPError):
        # A status code means the server is there and formed a reply.
        return LIVE
    reason = getattr(exc, "reason", exc)
    if isinstance(reason, (socket.timeout, TimeoutError)):
        return BUSY
    if isinstance(reason, (ConnectionRefusedError, socket.gaierror)):
        return DOWN
    if isinstance(reason, OSError) and reason.errno in (
        61,   # ECONNREFUSED on macOS
        111,  # ECONNREFUSED on Linux
        10061,  # WSAECONNREFUSED on Windows
    ):
        return DOWN
    return BUSY


def check_orcaslicer_connection(use_cache: bool = True, timeout: float = 0.3) -> str:
    """Probe OrcaSlicer and return LIVE, BUSY or DOWN.

    The probe timeout is deliberately short so startup stays fast. That is only safe because a
    timeout no longer means "down": HttpServer runs a single io thread (HttpServer.cpp:210) and
    every handler blocks it inside run_on_main_thread, so a burst of calls serialises and this
    probe queues behind them. Under that load the old bool verdict said "not running" about an
    application that was answering, and cached it for CONNECTION_CACHE_TTL seconds.
    """
    global _connection_cache

    if use_cache:
        now = time.time()
        if now - _connection_cache["last_check"] < CONNECTION_CACHE_TTL:
            cached = _connection_cache["connected"]
            if cached is not None:
                if cached == LIVE:
                    note_live_contact()
                return cached

    try:
        req = urllib.request.Request(ORCAMCP_URL, method="GET")
        with urllib.request.urlopen(req, timeout=timeout) as response:
            # Any response, not just a 200, proves something answered.
            verdict = LIVE
    except Exception as exc:
        verdict = _verdict_for_exception(exc)
        log_debug(f"Liveness probe verdict {verdict}: {exc!r}")

    # This is the one place liveness is actually proven, so it is also the one place that notes
    # it -- every call site (start_orca's already-running check, the launch poll, both tools/list
    # fast paths) gets the notification hook for free instead of needing its own.
    #
    # A forwarded request that succeeds after a BUSY probe therefore does NOT note contact, even
    # though it proves the server is alive. Deliberate: the list_changed notification is deferred
    # to the next LIVE probe, which the following call makes, so a stale tool list is corrected
    # one call later rather than never.
    if verdict == LIVE:
        note_live_contact()

    # BUSY is a momentary state, so it is never cached: caching it is precisely how one timed-out
    # probe turned into three seconds of fabricated "not running" answers.
    if verdict in (LIVE, DOWN):
        _connection_cache = {"connected": verdict, "last_check": time.time()}
    return verdict


def with_bridge_tools(server_tools: list) -> list:
    """The list a client is served, online and offline alike: the bridge's own tools first
    (start_orca is the entry point while the app is down), then the app's."""
    bridge_names = {t["name"] for t in BRIDGE_TOOLS}
    return BRIDGE_TOOLS + [t for t in server_tools if t.get("name") not in bridge_names]


def get_full_tools_list() -> list:
    """The list served while the app is not running. Its tools answer with a "start the app"
    error until it is."""
    return with_bridge_tools(OFFLINE_SERVER_TOOLS)


def adopt_live_tools(response: dict) -> dict:
    """Add the bridge's own tools to a live tools/list response, and cache the result for the
    moments the app is down again."""
    global CACHED_TOOLS
    tools = response.get("result", {}).get("tools")
    if tools:
        tools = with_bridge_tools(tools)
        response["result"]["tools"] = tools
        CACHED_TOOLS = tools
        log_debug(f"Cached {len(tools)} tools (including the bridge's own)")
    return response


def settle_tools_list(response: dict) -> dict:
    """The answer to a forwarded tools/list. A live list gets the bridge's own tools added. A failed
    one -- as in the seconds after the app quits, while the liveness cache still says it is up --
    becomes the list the bridge serves while the app is down, never an error: a client can drop a
    server whose tools/list fails."""
    global _served_static_tools
    if "result" in response:
        return adopt_live_tools(response)
    log_debug(f"Forwarded tools/list failed ({response.get('error')}); serving the offline list")
    if CACHED_TOOLS is not None:
        tools = CACHED_TOOLS
    else:
        tools = get_full_tools_list()
        _served_static_tools = True
    return make_success_response(response.get("id"), {"tools": tools})


def call_start_orca(request_id, arguments: dict) -> dict:
    """start_orca: launch the app and wait for it. Answered here, since the app cannot launch itself."""
    result = launch_orcamcp()
    if result["success"]:
        return make_success_response(request_id, {
            "content": [{"type": "text", "text": result["message"]}],
            "isError": False
        })
    return make_success_response(request_id, make_tool_error_result(result["message"]))


# wait_for_slice. Its text is the C++ registration's, served from orcamcp_tools.json; these are the
# numbers that text states.
WAIT_FOR_SLICE_HEADROOM_S = 15           # the cap sits this far below ORCAMCP_TIMEOUT,
WAIT_FOR_SLICE_HEADROOM_SHARE = 0.25     # or this share of it when that is less (ORCAMCP_TIMEOUT < 60 s)
WAIT_FOR_SLICE_MIN_S = 1                 # the shortest wait; a cap below it leaves no room to wait
WAIT_FOR_SLICE_POLL_S = 1.5              # between two get_slicing_status calls
WAIT_FOR_SLICE_MIN_POLL_TIMEOUT_S = 1.0   # one poll's own HTTP timeout: at least this, so it can be answered,
WAIT_FOR_SLICE_MAX_POLL_TIMEOUT_S = 10.0  # and at most this; the last poll starts this long before the deadline
WAIT_FOR_SLICE_GONE_AFTER_S = 1.0         # refusals must last this long before the app counts as gone
# The outcomes the app's get_slicing_status reports in slice_run.outcome once a run is over.
FINISHED_SLICE_OUTCOMES = ("done", "ended_early", "incomplete")
# JSON-RPC error the app answers with while it quits (OrcaMCPJsonRpcError.hpp, McpShuttingDown).
APP_QUITTING_ERROR = -32002


class AppBusy(Exception):
    """A poll the app did not answer: too busy to within its timeout (it serves one request at a
    time), or the connection closed under the reply. Either way the wait goes on."""


class AppDown(Exception):
    """Nothing listens at ORCAMCP_URL, or the app answered that it is quitting (`quitting`)."""

    def __init__(self, quitting: bool = False):
        super().__init__()
        self.quitting = quitting


class AppGarbled(Exception):
    """A reply that is not JSON: a body cut short, as an app that dies mid-reply leaves it."""


class AppUnavailable(Exception):
    """A call that ends a wait with an error: the app refused it, or answered something unreadable."""


def wait_for_slice_cap() -> float:
    """The longest wait_for_slice waits. The bridge answers nothing else meanwhile -- not a ping, not a
    cancel -- so the wait always stays below ORCAMCP_TIMEOUT, the longest a user has said one call may
    take: 15 s below it, or a quarter below it when that is less, so a small timeout keeps headroom too."""
    return TIMEOUT - min(WAIT_FOR_SLICE_HEADROOM_S, TIMEOUT * WAIT_FOR_SLICE_HEADROOM_SHARE)


def parse_wait_timeout(value, cap: float) -> tuple:
    """(seconds, capped, error) for wait_for_slice's timeout_s. Absent means the cap. A numeric string
    is read as a number, as the app's own parameters are for a client with a stale schema."""
    if cap < WAIT_FOR_SLICE_MIN_S:
        return None, False, (f"ORCAMCP_TIMEOUT is {TIMEOUT} s, which leaves no room to wait: wait_for_slice "
                             f"needs it to be 2 s or more. Raise ORCAMCP_TIMEOUT, or poll get_slicing_status.")
    if value is None:
        return cap, False, None
    seconds = None
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        seconds = float(value)
    elif isinstance(value, str):
        try:
            seconds = float(value)
        except ValueError:
            pass
    if seconds is None or not math.isfinite(seconds) or seconds < WAIT_FOR_SLICE_MIN_S:
        return None, False, f"timeout_s must be a number of seconds, at least 1 (got {value!r})"
    return min(seconds, cap), seconds > cap, None


def call_app_tool(name: str, arguments: dict, timeout: float) -> dict:
    """One tools/call to the app, and its decoded result. Raises AppBusy for a call it did not answer,
    AppDown when nothing listens or the app is quitting, and AppUnavailable for the rest."""
    request = {"jsonrpc": "2.0", "id": name, "method": "tools/call",
               "params": {"name": name, "arguments": arguments}}
    try:
        reply = post_to_app(request, timeout)
    except urllib.error.HTTPError as e:
        raise AppUnavailable(f"OrcaSlicer answered with HTTP {e.code} ({e.reason}) at {ORCAMCP_URL}.")
    except (socket.timeout, TimeoutError):
        raise AppBusy()
    except urllib.error.URLError as e:
        if _verdict_for_exception(e) == DOWN:
            raise AppDown()
        raise AppBusy()
    except json.JSONDecodeError as e:
        raise AppGarbled(f"Invalid JSON response from OrcaSlicer: {e}")
    except (http.client.HTTPException, OSError):
        # The connection closed under the reply (RemoteDisconnected, a reset, an IncompleteRead): an app
        # that is going away does this, and so does one whose request thread dropped the socket.
        raise AppBusy()
    if "error" in reply:
        if reply["error"].get("code") == APP_QUITTING_ERROR:
            raise AppDown(quitting=True)
        raise AppUnavailable(f"{name} failed: {reply['error'].get('message', reply['error'])}")
    try:
        return json.loads(reply["result"]["content"][0]["text"])
    except (KeyError, IndexError, TypeError, json.JSONDecodeError):
        raise AppUnavailable(f"{name} answered with a result wait_for_slice cannot read.")


def poll_timeout(remaining: float) -> float:
    """One status poll's HTTP timeout: what is left of the wait, within the least a poll needs to be
    answered and WAIT_FOR_SLICE_MAX_POLL_TIMEOUT_S. The wait never starts a poll with less than the least
    left (next_poll_delay), so no poll carries it past its deadline."""
    return min(WAIT_FOR_SLICE_MAX_POLL_TIMEOUT_S, max(WAIT_FOR_SLICE_MIN_POLL_TIMEOUT_S, remaining))


def next_poll_delay(remaining: float):
    """How long to sleep before the next poll, or None when there is no time left for one: the last
    poll starts a full WAIT_FOR_SLICE_MIN_POLL_TIMEOUT_S before the deadline, so it can be answered and
    the wait still ends by then. A slice that ends just before the deadline is caught by that poll."""
    spare = remaining - WAIT_FOR_SLICE_MIN_POLL_TIMEOUT_S
    if spare < 0:
        return None
    return min(WAIT_FOR_SLICE_POLL_S, spare)


def slice_over(status) -> bool:
    """Whether a status says the slicing pipeline has nothing left to do: its busy, which also covers
    an export, an upload and a completion not yet taken in -- the same answer slice_all refuses on.
    An app from before busy existed is judged by is_slicing."""
    if status is None:
        return False
    return not status.get("busy", status.get("is_slicing", False))


def slice_outcome(status: dict) -> tuple:
    """(outcome, message) for a status that says nothing is slicing. The app judges a slice_all run
    itself (slice_run.outcome); without one, the selected plate's result is all there is to go on."""
    run = status.get("slice_run") or {}
    if run.get("outcome") in FINISHED_SLICE_OUTCOMES:
        return run["outcome"], run.get("message")
    if status.get("state") == "done":
        return "done", None
    return "not_slicing", ("Nothing is slicing, and the selected plate has no slice result: call slice_all "
                           "to start a slice.")


def timed_out_message(timeout_s: float, status, refused_at=None) -> str:
    """Why a wait ran out. `refused_at`, seconds into the wait, when its last polls were refused: the
    status then predates them, so it says nothing about now."""
    if refused_at is not None:
        return (f"OrcaSlicer stopped answering {refused_at:.1f} s into the wait (its connections were refused): "
                f"it may have quit or crashed. Call wait_for_slice again; it reports app_gone once that is certain.")
    if status is None:
        return (f"OrcaSlicer answered no status poll within {timeout_s:g} s: it serves one request at a "
                f"time and was busy. Call wait_for_slice again.")
    return f"Still slicing after {timeout_s:g} s. Call wait_for_slice again to keep waiting."


APP_GONE_MESSAGE = ("OrcaSlicer stopped answering during the wait: it quit or crashed, so the slice did not "
                    "finish. Call start_orca, then slice_all again.")


class PollHistory:
    """What the polls so far say about the app. A refusal can be a moment in which the app is busy
    elsewhere, so the app counts as gone only when refusals have lasted WAIT_FOR_SLICE_GONE_AFTER_S,
    or when it answered that it is quitting."""

    def __init__(self):
        self.answered = False        # it gave a clean answer
        self.seen = False            # something accepted a connection: a clean answer or a busy poll
        self.refused_since = None    # when the current run of refusals began
        self.stale_since = None      # when the first refusal since the last clean answer came: from then
                                     # on the last status is from before, and says nothing about now

    def answer(self):
        self.answered = self.seen = True
        self.refused_since = self.stale_since = None

    def busy(self):
        # Something took the connection, so the app counts as there again; but it gave no answer, so
        # a status from before a refusal stays stale.
        self.seen = True
        self.refused_since = None

    def refused(self, now: float) -> bool:
        """Records a refusal; True once the app counts as gone."""
        if self.refused_since is None:
            self.refused_since = now
        if self.stale_since is None:
            self.stale_since = now
        return now - self.refused_since >= WAIT_FOR_SLICE_GONE_AFTER_S


class WaitState:
    """What one wait has learned so far."""

    def __init__(self):
        self.status = None   # the last status the app answered
        self.polls = 0       # polls it answered
        self.history = PollHistory()
        self.gone = False

    def poll(self, timeout: float):
        """One get_slicing_status poll with this HTTP timeout. Raises AppUnavailable for an app that was
        never there, or that failed the call."""
        try:
            self.status = call_app_tool("get_slicing_status", {}, timeout)
            self.polls += 1
            self.history.answer()
        except AppBusy:
            self.history.busy()
        except AppGarbled as e:
            # A reply cut short after the app had answered is the app dropping the connection; before
            # it had, it is a reply this bridge cannot read.
            if not self.history.answered:
                raise AppUnavailable(str(e))
            self.history.busy()
        except AppDown as e:
            if not self.history.seen:
                raise AppUnavailable(NOT_RUNNING_MESSAGE)
            self.gone = e.quitting or self.history.refused(time.monotonic())

    def finished(self) -> bool:
        return self.gone or (self.history.stale_since is None and slice_over(self.status))


def confirm_refusals(state: WaitState, deadline: float):
    """A wait whose last polls were refused ends with one more poll, WAIT_FOR_SLICE_GONE_AFTER_S after
    the first refusal, so it can say app_gone -- or read the app's answer if it is back -- rather than
    report a status from before the refusals. That poll may run past the deadline into half the cap's
    headroom below ORCAMCP_TIMEOUT, never further; when it cannot fit there, it is not made."""
    headroom_end = deadline + (TIMEOUT - wait_for_slice_cap()) / 2
    first_refusal = state.history.refused_since or state.history.stale_since
    confirm_at = first_refusal + WAIT_FOR_SLICE_GONE_AFTER_S
    if confirm_at > headroom_end - 0.05:
        return
    time.sleep(max(0.0, confirm_at - time.monotonic()))
    state.poll(timeout=max(0.05, headroom_end - time.monotonic()))


def run_wait_for_slice(timeout_s: float) -> dict:
    """Poll get_slicing_status until nothing is slicing, the app goes away, or timeout_s has passed, and
    report which. Raises AppUnavailable for an app that was never there, or that failed the call."""
    started = time.monotonic()
    deadline = started + timeout_s
    state = WaitState()
    while True:
        state.poll(timeout=poll_timeout(deadline - time.monotonic()))
        delay = next_poll_delay(deadline - time.monotonic())
        if state.finished() or delay is None:
            break
        time.sleep(delay)
    if not state.finished() and state.history.stale_since is not None:
        confirm_refusals(state, deadline)

    refused_at = state.history.stale_since
    if state.gone:
        outcome, message = "app_gone", APP_GONE_MESSAGE
    elif state.finished():
        outcome, message = slice_outcome(state.status)
    else:
        outcome, message = "timed_out", timed_out_message(timeout_s, state.status,
                                                          None if refused_at is None else refused_at - started)
    report = {"status": "success", "outcome": outcome, "timed_out": outcome == "timed_out"}
    if message:
        report["message"] = message
    report.update(waited_s=round(time.monotonic() - started, 1), polls=state.polls, slicing_status=state.status)
    return report


def call_wait_for_slice(request_id, arguments: dict) -> dict:
    """wait_for_slice: answered here, because a wait inside the app would stall every other call."""
    cap = wait_for_slice_cap()
    timeout_s, capped, error = parse_wait_timeout((arguments or {}).get("timeout_s"), cap)
    if error:
        return make_success_response(request_id, make_tool_error_result(error))
    try:
        report = run_wait_for_slice(timeout_s)
    except AppUnavailable as e:
        return make_success_response(request_id, make_tool_error_result(str(e)))
    report.update(timeout_s=timeout_s, timeout_cap_s=cap)
    if capped:
        report["timeout_capped"] = True
    return make_success_response(request_id, {
        "content": [{"type": "text", "text": json.dumps(report)}],
        "isError": False
    })


# The bridge's own tools. Their text is in orcamcp_tools.json's bridge_tools; every name there has a
# handler here and nothing else does (scripts/tests/test_bridge_tool_lists.py).
BRIDGE_HANDLERS = {
    "start_orca": call_start_orca,
    "wait_for_slice": call_wait_for_slice,
}


# JSON-RPC's invalid-params error, which a tool call the tool's schema refuses gets, as in the app.
INVALID_PARAMS_ERROR = -32602


def json_type_name(value) -> str:
    """The JSON name of a decoded value's type, as the app's messages spell it."""
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, (int, float)):
        return "number"
    if isinstance(value, str):
        return "string"
    return "array" if isinstance(value, list) else "object"


def _named(names: list, noun: str, quoted: bool) -> str:
    listed = ", ".join(f'"{n}"' if quoted else n for n in names)
    return f"{noun}{'s' if len(names) != 1 else ''} {listed}"


def argument_error(name: str, schema: dict, arguments) -> str | None:
    """Why a call to one of the bridge's own tools does not fit its schema, the inputSchema from
    orcamcp_tools.json, worded as the app words it for its tools (OrcaMCPToolArguments.cpp): arguments
    that are not an object, an argument the schema does not declare, or a required one left out.

    Only the top level is checked: no bridge tool takes a nested object (scripts/tests holds that), so
    the app's walk through nested objects has nothing to look at here. None when the call fits."""
    if not isinstance(arguments, dict):
        return f"{name}'s arguments must be a JSON object of named arguments; got {json_type_name(arguments)}."
    declared = schema.get("properties") or {}
    required = [r for r in schema.get("required") or [] if isinstance(r, str)]
    unknown = sorted(k for k in arguments if k not in declared) if schema.get("additionalProperties") is False else []
    missing = [r for r in required if r not in arguments]
    if not unknown and not missing:
        return None
    parts = []
    if unknown:
        parts.append("has no " + _named(unknown, "argument", quoted=True))
    if missing:
        parts.append("is missing its required " + _named(missing, "argument", quoted=True))
    taken = required + sorted(k for k in declared if k not in required)
    listed = f"Its arguments: {', '.join(taken)}." if taken else "It takes no arguments."
    return f"{name} {' and '.join(parts)}. {listed}"


def bridge_tool_schema(name: str) -> dict:
    """The inputSchema orcamcp_tools.json gives one of the bridge's own tools."""
    return next((t["inputSchema"] for t in BRIDGE_TOOLS if t["name"] == name), {})


def call_bridge_tool(request_id, name: str, handler, arguments) -> dict:
    """One of the bridge's own tools, once its arguments fit its schema; absent and null mean none."""
    arguments = {} if arguments is None else arguments
    error = argument_error(name, bridge_tool_schema(name), arguments)
    if error:
        log_debug(f"{name} refused: {error}")
        return make_error_response(request_id, INVALID_PARAMS_ERROR, error)
    return handler(request_id, arguments)


def handle_local_request(request: dict) -> dict | None:
    """
    Handle requests locally when OrcaSlicer isn't available.
    Returns None if the request should be forwarded to OrcaSlicer.
    """
    global _served_static_tools

    method = request.get("method", "")
    request_id = request.get("id", 0)
    params = request.get("params", {})

    # These methods are ALWAYS handled locally first to ensure fast MCP handshake
    # This prevents connection timeouts during initialization
    if method == "initialize":
        # Store client's protocol version for potential use
        client_protocol = params.get("protocolVersion", "2024-11-05")
        log_debug(f"Initialize from client with protocol {client_protocol}")
        return make_success_response(request_id, {
            "serverInfo": SERVER_INFO,
            "capabilities": {
                # The static list this bridge may serve first is not necessarily current, so the
                # client must be willing to be told it changed.
                "tools": {"listChanged": True}
            },
            "protocolVersion": client_protocol  # Echo client's version for compatibility
        })

    if method == "notifications/initialized":
        # This is a notification - no response needed but we must not block
        log_debug("Received initialized notification")
        return {"_no_response": True}  # Special marker - don't send any response

    if method == "ping":
        return make_success_response(request_id, {})

    # The bridge's own tools are always answered here, whether the app runs or not
    if method == "tools/call":
        name = params.get("name", "")
        handler = BRIDGE_HANDLERS.get(name)
        if handler is not None:
            return call_bridge_tool(request_id, name, handler, params.get("arguments"))

    # Optimization: For tools/list during initial startup (no cached tools),
    # return minimal list immediately without slow connection check
    if method == "tools/list" and CACHED_TOOLS is None:
        # First time - try a quick check, but return the static list fast if nothing answers
        if check_orcaslicer_connection(timeout=0.1) != LIVE:
            log_debug("Quick startup: returning static tools list")
            _served_static_tools = True
            return make_success_response(request_id, {"tools": get_full_tools_list()})
        # Connected - let it through to get full tools list
        return None

    # For other methods, only a DOWN verdict is answered locally. A BUSY server is forwarded to:
    # the real request has the full ORCAMCP_TIMEOUT to be answered, and a real error from a real
    # attempt is worth more to a caller than a guess made from a 0.3-second probe.
    if check_orcaslicer_connection() != DOWN:
        return None

    # Handle methods locally when OrcaSlicer is offline
    log_debug(f"OrcaSlicer offline - handling {method} locally")

    if method == "tools/list":
        # CACHED_TOOLS is guaranteed set by this point: the "CACHED_TOOLS is None" branch above
        # already handles (and returns from) every tools/list call before it is populated.
        return make_success_response(request_id, {"tools": CACHED_TOOLS})

    elif method == "tools/call":
        # start_orca is handled above, so any tool call here is for an unavailable tool
        return make_success_response(request_id, make_tool_error_result(NOT_RUNNING_MESSAGE))

    # For other methods, return a graceful error
    return make_success_response(request_id, make_tool_error_result(
        f"OrcaMCP is not running. Cannot process '{method}' request."
    ))


def normalize_paths_for_windows(request: dict) -> dict:
    """
    Normalize file paths in tool call arguments for Windows.
    Converts forward slashes to backslashes for path parameters.
    """
    import platform
    if platform.system() != "Windows":
        return request

    # Only process tools/call requests
    if request.get("method") != "tools/call":
        return request

    params = request.get("params", {})
    arguments = params.get("arguments", {})
    if not arguments:
        return request

    # Path parameter names used by OrcaMCP tools
    path_params = ["file_path", "output_path", "path"]

    modified = False
    for param in path_params:
        if param in arguments and isinstance(arguments[param], str):
            # Convert forward slashes to backslashes
            original = arguments[param]
            normalized = original.replace("/", "\\")
            if normalized != original:
                arguments[param] = normalized
                log_debug(f"Normalized path: {original} -> {normalized}")
                modified = True

    return request


def post_to_app(request: dict, timeout: float) -> dict:
    """POST one JSON-RPC request to OrcaSlicer and return its decoded reply. Raises whatever urlopen
    and json raise: send_request turns those into JSON-RPC errors, wait_for_slice into its own."""
    req = urllib.request.Request(
        ORCAMCP_URL,
        data=json.dumps(request).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Accept": "application/json",
        },
        method="POST"
    )
    log_debug(f"Sending request: {request.get('method', 'unknown')}")
    with urllib.request.urlopen(req, timeout=timeout) as response:
        response_data = response.read().decode("utf-8")
    log_debug(f"Response received: {len(response_data)} bytes")
    return json.loads(response_data)


def send_request(request: dict) -> dict:
    """Send JSON-RPC request to OrcaSlicer HTTP server"""
    request_id = request.get("id", 0)

    # Normalize paths for Windows before sending
    request = normalize_paths_for_windows(request)

    try:
        result = post_to_app(request, TIMEOUT)
        # Ensure response has proper id
        if "id" not in result or result["id"] is None:
            result["id"] = request_id
        return result
    except urllib.error.HTTPError as e:
        log_debug(f"HTTP error: {e}")
        return make_error_response(
            request_id,
            -32000,
            f"OrcaSlicer answered with HTTP {e.code} ({e.reason}) at {ORCAMCP_URL}."
        )
    except (socket.timeout, TimeoutError):
        log_debug(f"Request timed out after {TIMEOUT}s")
        return make_error_response(
            request_id,
            -32000,
            f"OrcaSlicer did not answer within {TIMEOUT}s. It serves one request at a time, so a "
            f"batch of calls queues; a slice or a render can also outlast the timeout. Wait and "
            f"retry, or raise ORCAMCP_TIMEOUT. This is not a sign that it stopped running."
        )
    except urllib.error.URLError as e:
        verdict = _verdict_for_exception(e)
        log_debug(f"Connection error ({verdict}): {e}")
        if verdict == DOWN:
            return make_error_response(
                request_id,
                -32000,
                f"Nothing is listening at {ORCAMCP_URL}. OrcaMCP is not running -- use the "
                f"'start_orca' tool. Error: {str(e)}"
            )
        return make_error_response(
            request_id,
            -32000,
            f"OrcaSlicer is reachable but the request did not complete: {str(e)}. Retry, or raise "
            f"ORCAMCP_TIMEOUT if a long operation is running."
        )
    except json.JSONDecodeError as e:
        log_debug(f"JSON decode error: {e}")
        return make_error_response(
            request_id,
            -32700,
            f"Invalid JSON response from OrcaSlicer: {str(e)}"
        )
    except Exception as e:
        log_debug(f"Unexpected error: {e}")
        return make_error_response(
            request_id,
            -32603,
            f"Internal error: {str(e)}"
        )


def main():
    """Main loop - read from stdin, forward to HTTP, write to stdout"""
    log_debug(f"Starting OrcaMCP Bridge")
    log_debug(f"Target: {ORCAMCP_URL}")

    # Read JSON-RPC messages from stdin (one per line)
    for line in sys.stdin:
        request = None
        try:
            line = line.strip()
            if not line:
                continue

            log_debug(f"Received: {line[:100]}...")

            try:
                request = json.loads(line)
            except json.JSONDecodeError as e:
                # Invalid JSON - send error response
                error_response = make_error_response(0, -32700, f"Parse error: {str(e)}")
                print(json.dumps(error_response), flush=True)
                continue

            # Try to handle locally if OrcaSlicer is offline
            local_response = handle_local_request(request)
            if local_response is not None:
                # Check for special "no response" marker (for notifications)
                if isinstance(local_response, dict) and local_response.get("_no_response"):
                    log_debug("Notification handled, no response sent")
                    continue
                print(json.dumps(local_response), flush=True)
                log_debug(f"Sent local response for {request.get('method', 'unknown')}")
                emit_tools_changed_notification()
                continue

            # Forward request to OrcaSlicer HTTP server
            response = send_request(request)

            # A tools/list gets the bridge's own tools added, or the offline list if the app failed it
            method = request.get("method", "")
            if method == "tools/list":
                response = settle_tools_list(response)

            # Send response to stdout
            print(json.dumps(response), flush=True)
            log_debug(f"Sent response for {method}")
            emit_tools_changed_notification()

        except Exception as e:
            # Catch any unexpected errors to prevent server crash
            log_debug(f"Error processing request: {e}")
            print(f"Error in main loop: {e}", file=sys.stderr)
            # Try to send an error response, under the request's own id: a client waits for that id
            try:
                request_id = request.get("id") if isinstance(request, dict) else None
                error_response = make_error_response(request_id, -32603, f"Internal error: {str(e)}")
                print(json.dumps(error_response), flush=True)
            except Exception:
                pass  # If we can't even send an error, just continue


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        log_debug("Interrupted")
        sys.exit(0)
    except BrokenPipeError:
        # Client closed connection - this is normal
        log_debug("Client disconnected (broken pipe)")
        sys.exit(0)
    except Exception as e:
        # Always log fatal errors to stderr so they appear in MCP logs
        print(f"[orcamcp-bridge] Fatal error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc(file=sys.stderr)
        sys.exit(1)

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
import datetime
import http.client
import json
import math
import os
import socket
import subprocess
import time
import urllib.parse
import urllib.request
import urllib.error
import uuid

# Every tool's name, description and schema, generated from the app's tool registry and checked
# against it by tests/slic3rutils/test_mcp_tool_list.cpp. The bridge serves its server_tools while
# the app is not running, and takes its own tools (bridge_tools, e.g. start_orca) from it whether
# the app runs or not, so a client sees the same names and descriptions before and after the app
# starts. The server instructions initialize answers come from it too. None of that text is written
# here; the one exception is the start_orca offered when the file itself is unusable
# (fallback_bridge_tools).
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


def _instructions(manifest) -> str | None:
    """The manifest's server instructions, or None when it has none: initialize then leaves them out."""
    instructions = manifest.get("instructions")
    return instructions if isinstance(instructions, str) and instructions else None


# The MCP protocol version the bridge answers when its file names none: the one every OrcaMCP answered
# before the list was written there. The list itself is the app's (OrcaMCPProtocolVersions.hpp).
PROTOCOL_VERSIONS_WITHOUT_MANIFEST = ["2024-11-05"]


def _protocol_versions(manifest) -> list:
    """The MCP protocol versions OrcaMCP speaks, newest first, as the manifest lists them."""
    versions = manifest.get("protocol_versions")
    if isinstance(versions, list) and versions and all(isinstance(v, str) and v for v in versions):
        return versions
    return PROTOCOL_VERSIONS_WITHOUT_MANIFEST


def load_tools_manifest(path: str = TOOLS_FILE) -> tuple:
    """Return (server_tools, bridge_tools, instructions, protocol_versions, error): tools/list entries,
    the server instructions (None when the file has none) and the MCP protocol versions, newest first.

    Never raises: a bridge that dies lists nothing at all. An unreadable or malformed file gives no
    app tools, the fallback start_orca, no instructions, the oldest protocol version, and the reason.
    """
    try:
        with open(path, encoding="utf-8") as f:
            manifest = json.load(f)
        return (_list_entries(manifest, "server_tools"), _list_entries(manifest, "bridge_tools"),
                _instructions(manifest), _protocol_versions(manifest), None)
    except Exception as e:
        error = f"cannot read OrcaMCP's tool list {path}: {e}"
        return [], fallback_bridge_tools(error), None, PROTOCOL_VERSIONS_WITHOUT_MANIFEST, error


def install_tools_manifest(path: str = TOOLS_FILE):
    """Read the tool list the bridge serves. Runs once at import; tests point it at other files."""
    global OFFLINE_SERVER_TOOLS, BRIDGE_TOOLS, SERVER_INSTRUCTIONS, PROTOCOL_VERSIONS, TOOLS_MANIFEST_ERROR
    (OFFLINE_SERVER_TOOLS, BRIDGE_TOOLS, SERVER_INSTRUCTIONS, PROTOCOL_VERSIONS,
     TOOLS_MANIFEST_ERROR) = load_tools_manifest(path)
    if TOOLS_MANIFEST_ERROR:
        print(f"[orcamcp-bridge] {TOOLS_MANIFEST_ERROR}", file=sys.stderr)


install_tools_manifest()

# Configuration
# The environment the bridge reads its ORCAMCP_* settings from, here and when a tool runs. The tests load
# the bridge with their own, so no setting of the shell that runs them changes what they test.
ENV = os.environ
# Several OrcaMCP instances can run at once, each with its MCP server on its own port from 13618
# (OrcaMCPPortChoice.hpp). The bridge finds them in the instance registry and sends every call to the
# one this session chose (see "Instances" below). ORCAMCP_URL is where it looks before any is chosen:
# 13618, the port every OrcaMCP tries first and every older one listens on, or ORCAMCP_PORT's.
FIRST_MCP_PORT = 13618
ORCAMCP_HOST = ENV.get("ORCAMCP_HOST", "127.0.0.1")
PINNED_PORT = int(ENV["ORCAMCP_PORT"]) if ENV.get("ORCAMCP_PORT") else None
ORCAMCP_PORT = PINNED_PORT or FIRST_MCP_PORT
ORCAMCP_URL = f"http://{ORCAMCP_HOST}:{ORCAMCP_PORT}/mcp"
TIMEOUT = int(ENV.get("ORCAMCP_TIMEOUT", "120"))  # 2 minute default for slicing
# Where every running instance publishes an entry, <pid>.json (OrcaMCPInstanceRegistry.hpp).
INSTANCES_DIR = ENV.get("ORCAMCP_INSTANCES_DIR") or os.path.join(os.path.expanduser("~"), ".orcamcp", "instances")

# What a tool call gets while nothing answers at ORCAMCP_URL.
NOT_RUNNING_MESSAGE = "OrcaMCP is not running. Use the 'start_orca' tool to start it, then try again."

# Server info for when OrcaMCP isn't connected
# Version is omitted since we don't know the actual OrcaMCP version
SERVER_INFO = {
    "name": "orca-slicer",
    "version": "unknown (OrcaMCP not running)",
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
# The tool list the client was last served, and whether the instance now chosen lists other tools:
# switching to an instance of another build must tell the client its list changed.
_served_tools = None
_pending_list_changed = False

# The instance this session's calls go to (its registry entry, or an older OrcaMCP's "legacy" one),
# None until one is chosen; and when it last answered, which tells a restart from a sibling.
_selected = None
_last_heard = 0.0


def get_orcamcp_executable() -> str | None:
    """The OrcaMCP start_orca launches: ORCAMCP_APP_PATH, or the installed app. Never a build in a source
    folder: it would run on the user's real data folder, as a test build must not (the user, 2026-09-28)."""
    import platform
    system = platform.system()

    paths = []

    # First priority: environment variable
    custom_path = ENV.get("ORCAMCP_APP_PATH")
    if custom_path:
        paths.append(custom_path)

    if system == "Darwin":  # macOS
        paths.extend([
            "/Applications/OrcaMCP.app/Contents/MacOS/OrcaSlicer",
            os.path.expanduser("~/Applications/OrcaMCP.app/Contents/MacOS/OrcaSlicer"),
        ])
    elif system == "Windows":
        paths.extend([
            os.path.join(os.environ.get("ProgramFiles", "C:\\Program Files"), "OrcaMCP", "orca-mcp.exe"),
            os.path.join(os.environ.get("ProgramFiles(x86)", "C:\\Program Files (x86)"), "OrcaMCP", "orca-mcp.exe"),
            os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs", "OrcaMCP", "orca-mcp.exe"),
        ])
    else:  # Linux
        paths.extend([
            "/usr/bin/orca-mcp",
            "/usr/local/bin/orca-mcp",
            os.path.expanduser("~/.local/bin/orca-mcp"),
            "/opt/OrcaMCP/bin/orca-mcp",
        ])

    for path in paths:
        if path and os.path.isfile(path):
            log_debug(f"Found OrcaMCP executable: {path}")
            return path

    log_debug(f"OrcaMCP executable not found. Searched paths: {paths}")
    return None


LAUNCH_WAIT_S = 30  # how long start_orca waits for the instance it launched to answer

# The CA bundles the Linux build looks for when OpenSSL's own is missing, in its order (Http.cpp,
# CA_BUNDLES). Its OpenSSL's own is a path in the build machine's folders, so it always is.
SYSTEM_CA_BUNDLES = (
    "/etc/pki/tls/certs/ca-bundle.crt",        # Fedora/RHEL 6
    "/etc/ssl/certs/ca-certificates.crt",      # Debian/Ubuntu/Gentoo etc.
    "/usr/share/ssl/certs/ca-bundle.crt",
    "/usr/local/share/certs/ca-root-nss.crt",  # FreeBSD
    "/etc/ssl/cert.pem",
    "/etc/ssl/ca-bundle.pem",                  # OpenSUSE Tumbleweed
)


def agent_launch_variables(launch_id: str, environ, system: str) -> dict:
    """What an agent's launch adds to the environment OrcaMCP starts in.

    ORCAMCP_SKIP_CLOUD_LOGIN marks the launch as an agent's: nobody is at the screen, so the app waits on
    nothing a person must answer, from the keychain to its startup dialogs (see CLAUDE.md, environment
    variables). ORCAMCP_LAUNCH_ID is the token the instance records in its registry entry, by which
    wait_for_launched knows it whatever wrapper ORCAMCP_APP_PATH is. On Linux, SSL_CERT_FILE names the
    system's CA bundle when nothing names one: an OrcaMCP older than 2.5.0.8 asks about it before its MCP
    server starts, even on an agent's launch."""
    variables = {"ORCAMCP_SKIP_CLOUD_LOGIN": "1", "ORCAMCP_LAUNCH_ID": launch_id}
    if system not in ("Darwin", "Windows") and not environ.get("SSL_CERT_FILE"):
        bundle = next((path for path in SYSTEM_CA_BUNDLES if os.path.isfile(path)), None)
        if bundle:
            variables["SSL_CERT_FILE"] = bundle
    return variables


def launch_process(executable: str, launch_id: str):
    """Start `executable` detached from this process, as an agent's launch carrying `launch_id`. The
    process started, or None when LaunchServices starts it (macOS), whose `open` ends at once."""
    import platform
    system = platform.system()
    variables = agent_launch_variables(launch_id, os.environ, system)
    agent_env = dict(os.environ, **variables)
    log_debug(f"Launching OrcaMCP from: {executable}")
    if system == "Windows":
        # Windows: use CREATE_NEW_PROCESS_GROUP and DETACHED_PROCESS
        DETACHED_PROCESS = 0x00000008
        CREATE_NEW_PROCESS_GROUP = 0x00000200
        return subprocess.Popen([executable], creationflags=DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, close_fds=True,
                                env=agent_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    app_path = executable.replace("/Contents/MacOS/OrcaSlicer", "")
    if system == "Darwin" and app_path.endswith(".app"):
        # --env reaches the app through LaunchServices; a plain env= would not. -n starts a new process:
        # without it LaunchServices only brings forward a copy of the bundle that already runs.
        env_arguments = [argument for name, value in variables.items() for argument in ("--env", f"{name}={value}")]
        subprocess.Popen(["open", "-n", *env_arguments, app_path], close_fds=True)
        return None
    return subprocess.Popen([executable], start_new_session=True, env=agent_env, close_fds=True,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def same_program(a: str, b: str) -> bool:
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


def is_launched_instance(instance: dict, executable: str, launch_id: str) -> bool:
    """Whether a new instance is the one a launch started. One that records launch tokens (2.5.0.8 and
    later) is known by the token it was given, whatever wrapper launched it; "" says no agent did. One
    older runs `executable`. An OrcaMCP older than 2.5.0.6 (legacy) is weighed by wait_for_launched."""
    if "launch_id" in instance:
        return instance["launch_id"] == launch_id
    return not instance.get("legacy") and same_program(instance.get("executable", ""), executable)


def failed_exit_status(process):
    """The status the launched process ended with, when it ended with a failure; else None. A wrapper that
    starts the app in the background and ends with 0 has done its job: the app still comes."""
    status = process.poll() if process is not None else None
    return status if status else None


def default_log_folder(system: str = None) -> str:
    """Where OrcaMCP writes its log by default: the log folder of its data folder (GUI_App.cpp; CLAUDE.md, "Where
    the app's data lives"). A --datadir the launch passes moves it."""
    import platform
    system = system or platform.system()
    if system == "Darwin":
        base = os.path.expanduser("~/Library/Application Support")
    elif system == "Windows":
        base = ENV.get("APPDATA") or os.path.expanduser("~")
    else:
        base = ENV.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    return os.path.join(base, "OrcaMCP", "log")


def unanswered_launch(executable: str, process) -> tuple:
    """start_orca's (message, next_steps) for a launch whose instance did not answer: what the process did, as far
    as the bridge can see it, and where the app's log says why."""
    log = f"Its log says why: {default_log_folder()} (or the log folder of the data folder a --datadir names)."
    exit_status = failed_exit_status(process)
    if exit_status is not None:
        return f"OrcaMCP ({executable}) exited with status {exit_status} before it answered, so it is not running. {log}", []
    if process is not None and process.poll() is None:
        message = (f"OrcaMCP ({executable}) runs as pid {process.pid}, but its MCP server has not answered within "
                   f"{LAUNCH_WAIT_S}s: it may still be loading (a first start on a new data folder takes longer), or a "
                   f"dialog may be waiting at startup for someone to answer it. {log}")
    else:
        message = f"OrcaMCP ({executable}) was launched but did not answer within {LAUNCH_WAIT_S}s: it may still be starting. {log}"
    return message, [next_step("list_instances", "lists it once its MCP server answers; when it is the only one running, "
                                                  "this session's calls go to it")]


def wait_for_launched(executable: str, known: set, launch_id: str, process=None):
    """The instance a launch started: one not running before (`known` keys) that carries the launch's token,
    or runs `executable` when it is too old to record one; never a window of another program, another
    session's launch or the user's own that came up meanwhile. An installed OrcaMCP older than 2.5.0.6
    cannot say what it runs; a new one of those, answering on the first port where nothing did before, is
    taken. None when none comes up within LAUNCH_WAIT_S, or once `process` ended with a failure."""
    deadline = time.time() + LAUNCH_WAIT_S
    while time.time() < deadline:
        time.sleep(1)
        new = [i for i in discover_instances()[0] if instance_key(i) not in known]
        launched = [i for i in new if is_launched_instance(i, executable, launch_id)]
        older = [i for i in new if i.get("legacy")]
        if launched or older:
            return (launched or older)[0]
        if failed_exit_status(process) is not None:
            return None
    return None


def launch_answer(success: bool, status: str, message: str, instance=None, instances=(), next_steps=None) -> dict:
    """What launch_orcamcp returns: start_orca's report, and the two fields its caller reads."""
    report = {"status": status, "message": message}
    if instance is not None:
        report["instance"] = instance_summary(instance)
    others = [instance_summary(i) for i in instances if instance is None or instance_key(i) != instance_key(instance)]
    if others:
        report["other_instances"] = others
    if next_steps:
        report["next_steps"] = next_steps
    return {"success": success, "message": message, "report": report}


def launch_orcamcp(new_instance: bool = False) -> dict:
    """start_orca: launch OrcaMCP and choose the instance it started, unless one runs that this session
    can use: the one it chose, or the only one when it has chosen none. Several running and none chosen
    launches nothing: which to use is the agent's choice. new_instance launches another in any case."""
    instances, _ = discover_instances()
    if not new_instance:
        found = instance_to_use(instances)
        if found is not None:
            choose_instance(found)
            note_live_contact()
            return launch_answer(True, "already_running",
                                 f"OrcaMCP is already running: {describe_instance(found)}. This session uses it.",
                                 found, instances)
        if _selected is None and PINNED_PORT is None and len(instances) > 1:
            return launch_answer(False, "several_running",
                                 f"{len(instances)} OrcaMCP instances run and this session has not chosen one, so "
                                 f"none was launched. Choose one, or pass new_instance: true to launch another.",
                                 None, instances, choose_steps(instances, "several instances run"))

    executable = get_orcamcp_executable()
    if not executable:
        return launch_answer(False, "not_started",
                             "Could not find the installed OrcaMCP app. Install it, or set ORCAMCP_APP_PATH to "
                             "the OrcaMCP program to launch.", None, instances)
    launch_id = str(uuid.uuid4())
    try:
        process = launch_process(executable, launch_id)
    except Exception as e:
        return launch_answer(False, "not_started", f"Failed to launch OrcaMCP ({executable}): {e}", None, instances)

    launched = wait_for_launched(executable, {instance_key(i) for i in instances}, launch_id, process)
    if launched is None:
        message, steps = unanswered_launch(executable, process)
        return launch_answer(False, "not_started", message, None, instances, steps)
    choose_instance(launched)
    note_live_contact()
    return launch_answer(True, "started", f"OrcaMCP started: {describe_instance(launched)}. This session uses it.",
                         launched, instances)


def log_debug(message: str):
    """Log debug message to stderr (won't interfere with stdout protocol)"""
    if ENV.get("ORCAMCP_DEBUG"):
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


def is_notification(message) -> bool:
    """A JSON-RPC notification: a method and no id. JSON-RPC never answers one, and none asks OrcaMCP for
    anything, so the bridge keeps every one to itself (the app accepts them too: OrcaMCPTransport.hpp)."""
    return isinstance(message, dict) and "method" in message and "id" not in message


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
    """True once the instance this session switched to lists other tools than the client holds, and at
    most once for a static tool list served before a live server answered."""
    global _notified_tools_changed, _pending_list_changed
    if _pending_list_changed:
        _pending_list_changed = False
        return True
    if _notified_tools_changed or not _served_static_tools or not _live_contact:
        return False
    _notified_tools_changed = True
    return True


def served(tools: list) -> list:
    """`tools`, recorded as the list the client now holds: a switch to an instance whose list differs
    tells it so."""
    global _served_tools
    _served_tools = tools
    return tools


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
        req = urllib.request.Request(current_url(), method="GET")
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


# ---------------------------------------------------------------------------------------------------
# Instances. Several OrcaMCP instances can run at once, each with its MCP server on its own port from
# 13618 to 13627 (OrcaMCPPortChoice.hpp). Each publishes an entry, <pid>.json, in INSTANCES_DIR, and GET
# /mcp answers with the same identity (OrcaMCPInstanceRegistry.hpp). The bridge:
#   - finds them: the entries whose port answers with the entry's instance_id (a busy one by its live
#     pid), and whatever answers at ORCAMCP_URL without an entry: an OrcaMCP older than 2.5.0.6
#     ("legacy", which tells neither pid nor project), or one whose entry is in another folder;
#   - chooses one for the session: the one on ORCAMCP_PORT when that is set, and never another; else the
#     only one. With several and none chosen, a tool call is refused with the list; select_instance or
#     start_orca chooses. It never moves to another by itself, except to follow a restart (is_successor);
#   - asks the chosen port who answers before every call (call_refusal), and names the chosen instance on
#     the call, in params._meta["orcamcp/instance"]: an instance refuses a call meant for another with
#     -32004 before running it, and an OrcaMCP older than 2.5.0.6, which ignores the stamp, is never sent
#     one meant for another.
INSTANCE_META_KEY = "orcamcp/instance"
LEGACY_INSTANCE_ID = "legacy"   # how an older OrcaMCP is named on a call: any newer one refuses it
WRONG_INSTANCE_ERROR = -32004   # OrcaMCPJsonRpcError.hpp, WrongInstance
INSTANCE_PROBE_TIMEOUT_S = 0.3
TOOL_LIST_TIMEOUT_S = 5


def current_url() -> str:
    """Where calls go: the chosen instance, or ORCAMCP_URL until one is chosen."""
    return _selected["url"] if _selected else ORCAMCP_URL


def note_heard():
    """The chosen instance answered as itself. A restart of it is an instance started after this."""
    global _last_heard
    _last_heard = time.time()


def pid_alive(pid) -> bool:
    """Whether a process with this pid runs. Never os.kill(pid, 0) on Windows: signal 0 is CTRL_C_EVENT."""
    if not isinstance(pid, int) or isinstance(pid, bool) or pid <= 0:
        return False
    if os.name == "nt":
        import ctypes
        process_query_limited_information, still_active, error_access_denied = 0x1000, 259, 5
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.OpenProcess(process_query_limited_information, False, pid)
        if not handle:
            return kernel32.GetLastError() == error_access_denied  # it runs, as another user
        code = ctypes.c_ulong()
        alive = bool(kernel32.GetExitCodeProcess(handle, ctypes.byref(code))) and code.value == still_active
        kernel32.CloseHandle(handle)
        return alive
    try:
        os.kill(pid, 0)
    except PermissionError:
        return True  # it runs, as another user
    except OSError:
        return False
    return True


def owned_by_user(path: str) -> bool:
    """An entry another account wrote is not trusted: it could point this bridge at its own server."""
    if os.name == "nt":
        return True  # the user's profile folder is the user's alone, by its ACLs
    try:
        return os.stat(path).st_uid == os.getuid()
    except OSError:
        return False


def valid_entry(entry) -> bool:
    """An instance as the app describes it, in its registry entry or its GET /mcp answer."""
    return (isinstance(entry, dict) and isinstance(entry.get("instance_id"), str) and bool(entry["instance_id"])
            and all(isinstance(entry.get(key), int) and not isinstance(entry.get(key), bool) for key in ("pid", "port"))
            and isinstance(entry.get("url"), str))


def registry_entries() -> list:
    """Every readable entry in INSTANCES_DIR that the user wrote; none for a missing folder."""
    try:
        names = sorted(os.listdir(INSTANCES_DIR))
    except OSError:
        return []
    entries = []
    for name in names:
        path = os.path.join(INSTANCES_DIR, name)
        if not name.endswith(".json") or not owned_by_user(path):
            continue
        try:
            with open(path, encoding="utf-8") as f:
                entry = json.load(f)
        except (OSError, ValueError):
            continue
        if valid_entry(entry):
            entries.append(entry)
    return entries


def fetch_identity(url: str, timeout: float) -> tuple:
    """GET /mcp at `url`: (LIVE, its JSON answer or None), (BUSY, None) or (DOWN, None)."""
    try:
        with urllib.request.urlopen(urllib.request.Request(url, method="GET"), timeout=timeout) as response:
            body = response.read()
    except Exception as exc:
        return _verdict_for_exception(exc), None
    try:
        answer = json.loads(body.decode("utf-8"))
    except (ValueError, UnicodeDecodeError, AttributeError):
        return LIVE, None
    return LIVE, answer if isinstance(answer, dict) else None


def answer_identity(answer) -> dict | None:
    """The instance a GET /mcp answer names; None from an OrcaMCP that does not say (older than 2.5.0.6)."""
    instance = answer.get("instance") if isinstance(answer, dict) else None
    return instance if valid_entry(instance) else None


def legacy_instance(url: str, answer: dict) -> dict:
    """An OrcaMCP older than 2.5.0.6 answering at `url`: it tells its version, nothing more."""
    return {"instance_id": LEGACY_INSTANCE_ID, "legacy": True, "port": urllib.parse.urlparse(url).port, "url": url,
            "version": answer.get("version", ""), "state": "live"}


def instance_key(instance: dict) -> str:
    """Tells instances apart: by instance id, and an older OrcaMCP by its port."""
    return f"legacy:{instance['port']}" if instance.get("legacy") else instance["instance_id"]


def probe_entry(entry: dict, timeout: float):
    """The entry as a running instance, with its state ("live", or "busy": its one request thread is in
    a call), or None when it is stale: its port does not answer with its instance id, and its process
    is gone or its port refuses."""
    verdict, answer = fetch_identity(entry["url"], timeout)
    identity = answer_identity(answer)
    if verdict == LIVE and identity and identity["instance_id"] == entry["instance_id"]:
        return dict(identity, state="live")  # the answer's project is the freshest
    if verdict == BUSY and pid_alive(entry["pid"]):
        return dict(entry, state="busy")
    return None


def discover_instances(timeout: float = INSTANCE_PROBE_TIMEOUT_S) -> tuple:
    """(the running instances, by port; how many registry entries were stale)."""
    from concurrent.futures import ThreadPoolExecutor
    entries = registry_entries()
    with ThreadPoolExecutor(max_workers=len(entries) + 1) as pool:
        at_default = pool.submit(fetch_identity, ORCAMCP_URL, timeout)
        probed = list(pool.map(lambda entry: probe_entry(entry, timeout), entries))
        default_verdict, default_answer = at_default.result()
    instances = [instance for instance in probed if instance]
    stale = len(entries) - len(instances)
    if default_verdict == LIVE and isinstance(default_answer, dict) and default_answer.get("name") == "orca-slicer":
        identity = answer_identity(default_answer)
        if identity is None:
            instances.append(legacy_instance(ORCAMCP_URL, default_answer))
        elif identity["instance_id"] not in {i["instance_id"] for i in instances}:
            instances.append(dict(identity, state="live"))
    instances.sort(key=lambda instance: instance["port"])
    return instances, stale


def started_epoch(instance: dict):
    """When the instance started, in seconds since the epoch; None when it does not say."""
    try:
        started = datetime.datetime.strptime(instance["started_at"], "%Y-%m-%dT%H:%M:%S.%fZ")
    except (KeyError, TypeError, ValueError):
        return None
    return started.replace(tzinfo=datetime.timezone.utc).timestamp()


def is_successor(candidate: dict, previous: dict, last_heard: float) -> bool:
    """Whether `candidate` is `previous` restarted: the same program on the same data folder, started
    after the bridge last heard from `previous`, and with no other instance of that program on that
    folder running when it started (alone_at_start, from its registry entry). A window opened beside
    `previous` is not one, however alike and however late (another session's start_orca new_instance,
    the user opening a second window from Finder), nor is an older OrcaMCP."""
    if candidate.get("legacy") or previous.get("legacy") or candidate.get("alone_at_start") is not True:
        return False
    started = started_epoch(candidate)
    return (started is not None and started > last_heard and candidate.get("executable") == previous.get("executable")
            and candidate.get("data_dir") == previous.get("data_dir"))


def is_selected(instance: dict) -> bool:
    return _selected is not None and instance_key(_selected) == instance_key(instance)


def describe_instance(instance: dict) -> str:
    """One line naming an instance: what an agent needs to tell it from another."""
    if instance.get("legacy"):
        return (f"an OrcaMCP older than 2.5.0.6 on port {instance['port']} (version {instance.get('version') or 'unknown'}; "
                f"it tells neither its pid nor its project)")
    project = instance.get("project") or {}
    unsaved = ", unsaved changes" if project.get("unsaved") else ""
    name = f"project \"{project['name']}\"" if project.get("name") else "no project yet"
    return (f"pid {instance['pid']} on port {instance['port']}, {name}{unsaved}, "
            f"program {instance.get('executable', '')}, data folder {instance.get('data_dir', '')}")


def instance_summary(instance: dict, others=()) -> dict:
    """An instance as list_instances, select_instance and start_orca show it. `others` are the other
    running instances, for the ones on the same data folder."""
    if instance.get("legacy"):
        return {"port": instance["port"], "legacy": True, "version": instance.get("version", ""),
                "state": instance.get("state", "live"), "selected": is_selected(instance),
                "note": "An OrcaMCP older than 2.5.0.6: it tells neither its pid nor its project, and it does not "
                        "refuse a call meant for another instance."}
    summary = {key: instance.get(key) for key in ("pid", "port", "version", "executable", "data_dir", "started_at", "project")}
    summary.update(state=instance.get("state", "live"), selected=is_selected(instance))
    sharing = [other["pid"] for other in others if not other.get("legacy") and other.get("instance_id") != instance["instance_id"]
               and other.get("data_dir") == instance.get("data_dir")]
    if sharing:
        summary["shares_data_dir_with"] = sharing
    return summary


def next_step(tool: str, why: str, arguments: dict = None) -> dict:
    """One of next_steps, in the app's shape (OrcaMCPNextSteps.hpp): the tool, its arguments, and why."""
    step = {"tool": tool}
    if arguments:
        step["arguments"] = arguments
    step["why"] = why
    return step


def choose_steps(instances: list, why: str) -> list:
    """Choosing one of `instances`: select_instance naming the first that tells who it is, and
    list_instances for them all. An older OrcaMCP is never suggested: it cannot say whose window it is,
    and on 2026-09-27 the one on 13618 was the user's own."""
    steps = [next_step("list_instances", "every running instance, with its open project")]
    first = next((instance for instance in instances if not instance.get("legacy")), None)
    if first is not None:
        steps.insert(0, next_step("select_instance", why, {"pid": first["pid"]}))
    return steps


def tool_report(request_id, report: dict, is_error: bool = False) -> dict:
    """A bridge tool's JSON answer, as the app's tools give theirs."""
    return make_success_response(request_id, {"content": [{"type": "text", "text": json.dumps(report)}],
                                              "isError": is_error})


def instance_error(request_id, message: str, instances: list, next_steps: list) -> dict:
    """A call answered here, not run: why, the running instances, and what to call next."""
    report = {"status": "error", "message": message, "instances": [instance_summary(i, instances) for i in instances]}
    if next_steps:
        report["next_steps"] = next_steps
    return tool_report(request_id, report, is_error=True)


def fetch_tools(timeout: float = TOOL_LIST_TIMEOUT_S):
    """The chosen instance's tool list, with the bridge's own tools; None when it does not answer."""
    try:
        reply = post_to_app({"jsonrpc": "2.0", "id": "tools", "method": "tools/list", "params": {}}, timeout)
    except Exception:
        return None
    tools = (reply.get("result") or {}).get("tools") if isinstance(reply, dict) else None
    return with_bridge_tools(tools) if tools else None


def note_tools_after_switch() -> bool:
    """After a switch: whether the instance now chosen lists other tools than the client holds. If so the
    client is told (notifications/tools/list_changed), and reloads the list from it."""
    global CACHED_TOOLS, _pending_list_changed
    tools = fetch_tools()
    if tools is None:
        return False
    CACHED_TOOLS = tools
    if _served_tools is None or tools == _served_tools:
        return False
    _pending_list_changed = True
    return True


def choose_instance(instance: dict) -> bool:
    """From now on every call goes to `instance`. True when it replaced another whose build lists other
    tools: the client is then told to reload its list."""
    global _selected, _connection_cache
    previous = _selected
    _selected = dict(instance)
    _connection_cache = {"connected": None, "last_check": 0}
    note_heard()
    if previous is None or instance_key(previous) == instance_key(instance):
        return False
    return note_tools_after_switch()


def first_choice(instances: list):
    """The instance a session starts with: the one on ORCAMCP_PORT when that is set, and then only that
    one, never another when nothing answers there (a session pinned to a test build not up yet would
    drive the user's app); else the only one running. None when none qualifies, or several run: which
    to drive is then the agent's choice."""
    if PINNED_PORT is not None:
        return next((instance for instance in instances if instance["port"] == PINNED_PORT), None)
    return instances[0] if len(instances) == 1 else None


def choose_first_instance(timeout: float = INSTANCE_PROBE_TIMEOUT_S) -> list:
    """Chooses the first instance, if one is to be chosen (first_choice). Returns the running ones."""
    instances, _ = discover_instances(timeout)
    first = first_choice(instances)
    if first is not None:
        choose_instance(first)
    return instances


def instance_to_use(instances: list):
    """The running instance start_orca finds for this session: the one it chose, or the first choice
    when it has chosen none. None when that one is gone, or several run and none is chosen."""
    if _selected is not None:
        return next((instance for instance in instances if is_selected(instance)), None)
    return first_choice(instances)


def lost_instance_answer(request_id, what: str) -> dict:
    """The chosen instance is gone (`what` says how the bridge found out), and this call was not run. A
    restart of it is followed, and said; any other instance is the agent's choice, never taken for it."""
    lost = _selected
    instances, _ = discover_instances()
    successors = [instance for instance in instances if is_successor(instance, lost, _last_heard)]
    if len(successors) == 1:
        choose_instance(successors[0])
        return instance_error(request_id,
                              f"The OrcaMCP instance this session used ({describe_instance(lost)}) has restarted: "
                              f"this session now uses {describe_instance(successors[0])}. This call was not run, since "
                              f"the scene is now the restarted instance's: look at it, then call again.",
                              instances, [next_step("get_scene_info", "the restarted instance's scene")])
    if not instances:
        return instance_error(request_id,
                              f"The OrcaMCP instance this session used ({describe_instance(lost)}) {what}. No OrcaMCP "
                              f"is running now, and this call was not run.",
                              instances, [next_step("start_orca", "start OrcaMCP again")])
    return instance_error(request_id,
                          f"The OrcaMCP instance this session used ({describe_instance(lost)}) {what}. This call was "
                          f"not run, and no other instance is chosen in its place: choose one, or start a new one.",
                          instances, choose_steps(instances, "drive one of the instances still running") +
                          [next_step("start_orca", "launch a new OrcaMCP window instead", {"new_instance": True})])


def answers_as(answer, instance: dict) -> bool:
    """Whether a GET /mcp answer comes from `instance`: the same instance id, or, for an older OrcaMCP, an
    OrcaMCP answer with none. A newer instance's answer is never an older one's, nor the reverse."""
    identity = answer_identity(answer)
    if instance.get("legacy"):
        return identity is None and isinstance(answer, dict) and answer.get("name") == "orca-slicer"
    return identity is not None and identity["instance_id"] == instance["instance_id"]


def call_refusal(request_id):
    """A local answer when a tool call must not be forwarded: several instances run and none is chosen,
    nothing answers on a pinned port, or the chosen instance is gone. None when it may go ahead: to the
    chosen instance, or, with none running, to the "not running" answer."""
    if _selected is None:
        instances = choose_first_instance()
        if _selected is not None:
            return None
        if PINNED_PORT is not None:
            steps = [next_step("start_orca", "start OrcaMCP")]
            if instances:
                steps += choose_steps(instances, "drive one of the instances running instead")
            return instance_error(request_id,
                                  f"No OrcaMCP answers on port {PINNED_PORT}, the port ORCAMCP_PORT names, so this call "
                                  f"was not run. Another instance is never used in its place.", instances, steps)
        if len(instances) > 1:
            return instance_error(request_id,
                                  f"{len(instances)} OrcaMCP instances run, and this session has not chosen the one "
                                  f"its calls go to, so this call was not run.",
                                  instances, choose_steps(instances, "choose the instance this session drives"))
        return None
    # Is the chosen instance still the one on its port? Not merely something: an OrcaMCP older than 2.5.0.6
    # that took the port would ignore the call's stamp and run it.
    verdict, answer = fetch_identity(current_url(), INSTANCE_PROBE_TIMEOUT_S)
    if verdict == BUSY:
        return None  # it is in a call; the stamp keeps any newer instance from running this one
    if verdict == DOWN:
        return lost_instance_answer(request_id, "has quit or crashed")
    if not answers_as(answer, _selected):
        return lost_instance_answer(request_id, "no longer answers on its port: another program does")
    note_heard()
    return None


def settle_call(request: dict, response: dict) -> dict:
    """A forwarded tool call's answer. One that reached another instance than the chosen one was refused
    unrun (-32004): the chosen one is gone from its port."""
    error = response.get("error") if isinstance(response, dict) else None
    if request.get("method") == "tools/call" and isinstance(error, dict) and error.get("code") == WRONG_INSTANCE_ERROR \
            and _selected is not None:
        return lost_instance_answer(request.get("id"), "no longer answers on its port: another instance does")
    return response


def stamp_instance(request: dict) -> dict:
    """Name the chosen instance in a tools/call's params._meta, so no other instance runs it."""
    params = request.get("params")
    if request.get("method") != "tools/call" or not isinstance(params, dict) or _selected is None:
        return request
    meta = params.get("_meta")
    if not isinstance(meta, dict):
        meta = params["_meta"] = {}
    meta[INSTANCE_META_KEY] = _selected["instance_id"]
    return request


def summary_as_it_is(instance: dict, instances: list) -> dict:
    """`instance` as it is now: its summary among the running `instances`, or, once it has quit, the one it
    was chosen by, marked gone."""
    running = next((i for i in instances if instance_key(i) == instance_key(instance)), None)
    return instance_summary(running, instances) if running else dict(instance_summary(instance), state="gone")


def call_list_instances(request_id, arguments: dict) -> dict:
    """list_instances: every running instance, and the one this session uses."""
    instances, stale = discover_instances()
    report = {"status": "success", "instances": [instance_summary(i, instances) for i in instances],
              "using": None}
    if _selected is not None:
        report["using"] = summary_as_it_is(_selected, instances)
    if stale:
        report["stale_entries_ignored"] = stale
    if not instances:
        report["next_steps"] = [next_step("start_orca", "no OrcaMCP is running")]
    elif report["using"] is None and len(instances) > 1:
        report["next_steps"] = choose_steps(instances, "several instances run and this session has not chosen one")
    elif report["using"] is not None and report["using"]["state"] == "gone":
        report["next_steps"] = choose_steps(instances, "the instance this session used is gone")
    return tool_report(request_id, report)


SELECT_KEYS = ("pid", "port", "project")


def select_argument_error(arguments: dict):
    """Why select_instance's arguments cannot choose: it takes exactly one of pid, port or project."""
    given = [key for key in SELECT_KEYS if key in arguments]
    if len(given) != 1:
        return f"select_instance takes exactly one of pid, port or project; got {', '.join(given) or 'none'}."
    key, value = given[0], arguments[given[0]]
    if key == "project" and (not isinstance(value, str) or not value):
        return f"select_instance's project must be a project name or path; got {json.dumps(value)}."
    if key != "project" and (not isinstance(value, int) or isinstance(value, bool)):
        return f"select_instance's {key} must be an integer; got {json.dumps(value)}."
    return None


def matches(instance: dict, key: str, value) -> bool:
    """Whether `instance` is the one select_instance's `key` names: a pid, a port, or its open project's
    name (any case) or full path."""
    if key == "port":
        return instance["port"] == value
    if instance.get("legacy"):
        return False  # it tells neither
    if key == "pid":
        return instance["pid"] == value
    project = instance.get("project") or {}
    path = project.get("path") or ""
    return project.get("name", "").lower() == value.lower() or (bool(path) and os.path.normpath(path) == os.path.normpath(value))


def call_select_instance(request_id, arguments: dict) -> dict:
    """select_instance: send this session's calls to one running instance, confirmed by its identity."""
    error = select_argument_error(arguments)
    if error:
        return make_error_response(request_id, INVALID_PARAMS_ERROR, error)
    key = next(k for k in SELECT_KEYS if k in arguments)
    value = arguments[key]
    instances, _ = discover_instances()
    chosen = [instance for instance in instances if matches(instance, key, value)]
    if len(chosen) != 1:
        problem = "No running instance has" if not chosen else f"{len(chosen)} running instances have"
        steps = [next_step("list_instances", "every running instance, with its open project")] if instances else \
            [next_step("start_orca", "no OrcaMCP is running")]
        return instance_error(request_id, f"{problem} {key} {json.dumps(value)}. Nothing was changed.", instances, steps)
    previous = _selected
    changed = choose_instance(chosen[0])
    report = {"status": "success", "instance": instance_summary(_selected, instances),
              "previous": summary_as_it_is(previous, instances) if previous else None, "tool_list_changed": changed}
    if chosen[0]["state"] == "busy":
        report["note"] = ("It is busy with a call, so its identity was taken from its registry entry; each call "
                          "still names it, and it refuses any meant for another.")
    return tool_report(request_id, report)


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
        response["result"]["tools"] = served(tools)
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
    return make_success_response(response.get("id"), {"tools": served(tools)})


def call_start_orca(request_id, arguments: dict) -> dict:
    """start_orca: launch the app and wait for it. Answered here, since the app cannot launch itself."""
    new_instance = arguments.get("new_instance", False)
    if not isinstance(new_instance, bool):
        return make_error_response(request_id, INVALID_PARAMS_ERROR,
                                   f"start_orca's new_instance must be true or false; got {json.dumps(new_instance)}.")
    result = launch_orcamcp(new_instance=new_instance)
    text = json.dumps(result["report"]) if "report" in result else result["message"]
    return make_success_response(request_id, {"content": [{"type": "text", "text": text}],
                                              "isError": not result["success"]})


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
FINISHED_SLICE_OUTCOMES = ("done", "ended_early", "incomplete", "cancelled")
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
    request = stamp_instance({"jsonrpc": "2.0", "id": name, "method": "tools/call",
                              "params": {"name": name, "arguments": arguments}})
    try:
        reply = post_to_app(request, timeout)
    except urllib.error.HTTPError as e:
        raise AppUnavailable(f"OrcaSlicer answered with HTTP {e.code} ({e.reason}) at {current_url()}.")
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
        # Quitting, or gone from its port, which another instance now answers: either way it is gone.
        if reply["error"].get("code") in (APP_QUITTING_ERROR, WRONG_INSTANCE_ERROR):
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
    "list_instances": call_list_instances,
    "select_instance": call_select_instance,
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


def negotiated_protocol_version(requested) -> str:
    """MCP's version negotiation, as the app's (OrcaMCPProtocolVersions.hpp): the version the client asked
    for when OrcaMCP speaks it, otherwise the newest it speaks."""
    return requested if isinstance(requested, str) and requested in PROTOCOL_VERSIONS else PROTOCOL_VERSIONS[0]


def initialize_result(client_protocol) -> dict:
    """What initialize answers, app or no app: the app's server instructions, from orcamcp_tools.json,
    are the one text a client shows before any tool is loaded."""
    result = {
        "serverInfo": SERVER_INFO,
        "capabilities": {
            # The static list this bridge may serve first is not necessarily current, so the
            # client must be willing to be told it changed.
            "tools": {"listChanged": True}
        },
        "protocolVersion": negotiated_protocol_version(client_protocol)
    }
    if SERVER_INSTRUCTIONS:
        result["instructions"] = SERVER_INSTRUCTIONS
    return result


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
        client_protocol = params.get("protocolVersion") if isinstance(params, dict) else None
        log_debug(f"Initialize from client with protocol {client_protocol}")
        return make_success_response(request_id, initialize_result(client_protocol))

    if is_notification(request):
        # notifications/initialized, notifications/cancelled (the user stopped a tool call), ...: no answer,
        # and nothing to forward. An answer would carry id 0, which no request waits for.
        log_debug(f"Received notification {method}")
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
        if _selected is None:
            choose_first_instance(timeout=0.1)
        if check_orcaslicer_connection(timeout=0.1) != LIVE:
            log_debug("Quick startup: returning static tools list")
            _served_static_tools = True
            return make_success_response(request_id, {"tools": served(get_full_tools_list())})
        # Connected - let it through to get full tools list
        return None

    # A tool call goes to the instance this session chose; not at all while several run and none is
    # chosen, or when the chosen one is gone.
    if method == "tools/call":
        refusal = call_refusal(request_id)
        if refusal is not None:
            return refusal

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
        return make_success_response(request_id, {"tools": served(CACHED_TOOLS)})

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

    # Only named arguments carry paths. Anything else (a string or a list where the object belongs) is
    # forwarded untouched, so the app refuses it as invalid params (-32602) rather than this raising.
    params = request.get("params")
    arguments = params.get("arguments") if isinstance(params, dict) else None
    if not isinstance(arguments, dict) or not arguments:
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


# Where a forwarded tools/call tells the app how long a tool may wait for a job it starts.
WAIT_CAP_META_KEY = "orcamcp/wait_cap_s"


def stamp_wait_cap(request: dict) -> dict:
    """Tell the app, in a tools/call's params._meta, how long a tool may wait for a job it starts
    (arrange_objects, auto_orient, flatten_object and clone_object wait for their arrange or orient):
    wait_for_slice's cap, so the app answers before ORCAMCP_TIMEOUT, or 0 -- no wait -- under a timeout
    too short to wait in. The client's own _meta is kept; one that is not an object is replaced."""
    params = request.get("params")
    if request.get("method") != "tools/call" or not isinstance(params, dict):
        return request
    meta = params.get("_meta")
    if not isinstance(meta, dict):
        # One that is not an object carries nothing a client could have meant for the app; without the
        # cap the app would wait its own 105 s, past a shorter ORCAMCP_TIMEOUT.
        meta = params["_meta"] = {}
    cap = wait_for_slice_cap()
    meta[WAIT_CAP_META_KEY] = cap if cap >= WAIT_FOR_SLICE_MIN_S else 0
    return request


def post_to_app(request: dict, timeout: float) -> dict:
    """POST one JSON-RPC request to OrcaSlicer and return its decoded reply. Raises whatever urlopen
    and json raise: send_request turns those into JSON-RPC errors, wait_for_slice into its own."""
    req = urllib.request.Request(
        current_url(),
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
    request = stamp_wait_cap(request)
    request = stamp_instance(request)

    try:
        result = post_to_app(request, TIMEOUT)
        # Ensure response has proper id
        if "id" not in result or result["id"] is None:
            result["id"] = request_id
        if (result.get("error") or {}).get("code") != WRONG_INSTANCE_ERROR:
            note_heard()  # it answered as the instance the call named
        return result
    except urllib.error.HTTPError as e:
        log_debug(f"HTTP error: {e}")
        return make_error_response(
            request_id,
            -32000,
            f"OrcaSlicer answered with HTTP {e.code} ({e.reason}) at {current_url()}."
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
                f"Nothing is listening at {current_url()}. OrcaMCP is not running -- use the "
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
            response = settle_call(request, send_request(request))

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

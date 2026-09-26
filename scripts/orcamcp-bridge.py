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
import json
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


# The bridge's own tools. Their text is in orcamcp_tools.json's bridge_tools; every name there has a
# handler here and nothing else does (scripts/tests/test_bridge_tool_lists.py).
BRIDGE_HANDLERS = {
    "start_orca": call_start_orca,
}


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
        handler = BRIDGE_HANDLERS.get(params.get("name", ""))
        if handler is not None:
            return handler(request_id, params.get("arguments", {}))

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
        return make_success_response(request_id, make_tool_error_result(
            f"OrcaMCP is not running. Use the 'start_orca' tool to start it, then try again."
        ))

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


def send_request(request: dict) -> dict:
    """Send JSON-RPC request to OrcaSlicer HTTP server"""
    request_id = request.get("id", 0)

    # Normalize paths for Windows before sending
    request = normalize_paths_for_windows(request)

    data = json.dumps(request).encode("utf-8")

    req = urllib.request.Request(
        ORCAMCP_URL,
        data=data,
        headers={
            "Content-Type": "application/json",
            "Accept": "application/json",
        },
        method="POST"
    )

    try:
        log_debug(f"Sending request: {request.get('method', 'unknown')}")
        with urllib.request.urlopen(req, timeout=TIMEOUT) as response:
            response_data = response.read().decode("utf-8")
            log_debug(f"Response received: {len(response_data)} bytes")
            result = json.loads(response_data)
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
            # Try to send an error response
            try:
                error_response = make_error_response(0, -32603, f"Internal error: {str(e)}")
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

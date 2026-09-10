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

# Import full tools schema (generated from OrcaMCP server)
# This ensures clients always have correct schemas even when OrcaMCP is offline
try:
    from tools_schema import FULL_TOOLS_LIST
except ImportError:
    # Fallback if tools_schema.py is not available
    FULL_TOOLS_LIST = []

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
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL
            )
        elif platform.system() == "Darwin":
            # macOS: use 'open' command for .app bundles
            app_path = executable.replace("/Contents/MacOS/OrcaSlicer", "")
            if app_path.endswith(".app"):
                subprocess.Popen(["open", app_path], close_fds=True)
            else:
                subprocess.Popen(
                    [executable],
                    start_new_session=True,
                    close_fds=True,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL
                )
        else:
            # Linux: start new session
            subprocess.Popen(
                [executable],
                start_new_session=True,
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
            if _connection_cache["connected"] is not None:
                return _connection_cache["connected"]

    try:
        req = urllib.request.Request(ORCAMCP_URL, method="GET")
        with urllib.request.urlopen(req, timeout=timeout) as response:
            # Any response, not just a 200, proves something answered.
            verdict = LIVE
    except Exception as exc:
        verdict = _verdict_for_exception(exc)
        log_debug(f"Liveness probe verdict {verdict}: {exc!r}")

    # BUSY is a momentary state, so it is never cached: caching it is precisely how one timed-out
    # probe turned into three seconds of fabricated "not running" answers.
    if verdict in (LIVE, DOWN):
        _connection_cache = {"connected": verdict, "last_check": time.time()}
    return verdict


def get_full_tools_list() -> list:
    """Return complete tools list with correct schemas.

    Uses FULL_TOOLS_LIST from tools_schema.py (generated from OrcaMCP server)
    plus start_orca (handled by bridge). This ensures clients always have
    correct schemas even when OrcaMCP is offline - tools will return helpful
    errors when called if OrcaMCP isn't running.
    """
    # start_orca is always first - it's the entry point when OrcaMCP is offline
    start_orca = {
        "name": "start_orca",
        "description": "Start the OrcaMCP application. Use this first when OrcaMCP is not running. The tool will launch OrcaMCP and wait for it to be ready. Once started, all other tools become available.",
        "inputSchema": {
            "type": "object",
            "properties": {},
            "required": []
        }
    }

    # Return start_orca + all tools from schema (excluding any duplicate start_orca)
    other_tools = [t for t in FULL_TOOLS_LIST if t.get("name") != "start_orca"]
    return [start_orca] + other_tools


def handle_local_request(request: dict) -> dict | None:
    """
    Handle requests locally when OrcaSlicer isn't available.
    Returns None if the request should be forwarded to OrcaSlicer.
    """
    global CACHED_TOOLS

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
                "tools": {}
            },
            "protocolVersion": client_protocol  # Echo client's version for compatibility
        })

    if method == "notifications/initialized":
        # This is a notification - no response needed but we must not block
        log_debug("Received initialized notification")
        return {"_no_response": True}  # Special marker - don't send any response

    if method == "ping":
        return make_success_response(request_id, {})

    # Handle start_orca tool - always handled locally (works whether connected or not)
    if method == "tools/call":
        tool_name = params.get("name", "")
        if tool_name == "start_orca":
            result = launch_orcamcp()
            if result["success"]:
                return make_success_response(request_id, {
                    "content": [{"type": "text", "text": result["message"]}],
                    "isError": False
                })
            else:
                return make_success_response(request_id, make_tool_error_result(result["message"]))

    # Optimization: For tools/list during initial startup (no cached tools),
    # return minimal list immediately without slow connection check
    if method == "tools/list" and CACHED_TOOLS is None:
        # First time - try a quick check, but return the static list fast if nothing answers
        if check_orcaslicer_connection(timeout=0.1) != LIVE:
            log_debug("Quick startup: returning static tools list")
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
        # Return cached tools if available, otherwise minimal list
        tools = CACHED_TOOLS if CACHED_TOOLS else get_full_tools_list()
        return make_success_response(request_id, {"tools": tools})

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
    global CACHED_TOOLS

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
                continue

            # Forward request to OrcaSlicer HTTP server
            response = send_request(request)

            # Cache tools list on successful tools/list response
            # Also inject start_orca tool (handled by bridge, not OrcaSlicer)
            method = request.get("method", "")
            if method == "tools/list" and "result" in response:
                tools = response.get("result", {}).get("tools")
                if tools:
                    # Add start_orca tool to the list (handled by bridge)
                    start_orca_tool = {
                        "name": "start_orca",
                        "description": "Start the OrcaMCP application. Use this when OrcaMCP is not running. The tool will launch OrcaMCP and wait for it to be ready.",
                        "inputSchema": {
                            "type": "object",
                            "properties": {},
                            "required": []
                        }
                    }
                    # Insert at beginning so it's visible
                    tools = [start_orca_tool] + [t for t in tools if t.get("name") != "start_orca"]
                    response["result"]["tools"] = tools
                    CACHED_TOOLS = tools
                    log_debug(f"Cached {len(tools)} tools (including start_orca)")

            # Send response to stdout
            print(json.dumps(response), flush=True)
            log_debug(f"Sent response for {method}")

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

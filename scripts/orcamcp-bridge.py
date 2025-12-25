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
import urllib.request
import urllib.error

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


def check_orcaslicer_connection() -> bool:
    """Check if OrcaSlicer is reachable"""
    try:
        req = urllib.request.Request(
            ORCAMCP_URL,
            method="GET"
        )
        with urllib.request.urlopen(req, timeout=2) as response:
            return response.status == 200
    except Exception:
        return False


def get_minimal_tools_list() -> list:
    """Return a minimal tools list when OrcaMCP isn't connected"""
    # This is a subset of tools that helps users understand what's available
    return [
        {
            "name": "get_server_info",
            "description": "Get OrcaMCP server information and connection status",
            "inputSchema": {
                "type": "object",
                "properties": {},
                "required": []
            }
        },
        {
            "name": "get_scene_info",
            "description": "Get detailed information about the current 3D printing scene including objects, plates, and print settings. Requires OrcaMCP to be running.",
            "inputSchema": {
                "type": "object",
                "properties": {},
                "required": []
            }
        },
        {
            "name": "load_model",
            "description": "Load a 3D model file (STL, OBJ, 3MF, STEP) into OrcaMCP. Requires OrcaMCP to be running.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "path": {
                        "type": "string",
                        "description": "Path to the 3D model file"
                    }
                },
                "required": ["path"]
            }
        },
        {
            "name": "slice_all",
            "description": "Slice all plates in the current project. Requires OrcaMCP to be running.",
            "inputSchema": {
                "type": "object",
                "properties": {},
                "required": []
            }
        }
    ]


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

    # For other methods, check if OrcaSlicer is available
    is_connected = check_orcaslicer_connection()

    if is_connected:
        # OrcaSlicer is available - let request go through
        return None

    # Handle methods locally when OrcaSlicer is offline
    log_debug(f"OrcaSlicer offline - handling {method} locally")

    if method == "tools/list":
        # Return cached tools if available, otherwise minimal list
        tools = CACHED_TOOLS if CACHED_TOOLS else get_minimal_tools_list()
        return make_success_response(request_id, {"tools": tools})

    elif method == "tools/call":
        tool_name = request.get("params", {}).get("name", "unknown")
        return make_success_response(request_id, make_tool_error_result(
            f"OrcaMCP is not running. Please start OrcaMCP to use the '{tool_name}' tool.\n\n"
            f"To start OrcaMCP:\n"
            f"- macOS: Open /Applications/OrcaMCP.app\n"
            f"- Windows: Run OrcaMCP from Start Menu\n"
            f"- Linux: Run orcamcp from terminal\n\n"
            f"The MCP server will automatically connect once OrcaMCP is running."
        ))

    # For other methods, return a graceful error
    return make_success_response(request_id, make_tool_error_result(
        f"OrcaMCP is not running. Cannot process '{method}' request."
    ))


def send_request(request: dict) -> dict:
    """Send JSON-RPC request to OrcaSlicer HTTP server"""
    request_id = request.get("id", 0)
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
    except urllib.error.URLError as e:
        log_debug(f"Connection error: {e}")
        return make_error_response(
            request_id,
            -32000,
            f"Cannot connect to OrcaSlicer at {ORCAMCP_URL}. Is OrcaSlicer running? Error: {str(e)}"
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
            method = request.get("method", "")
            if method == "tools/list" and "result" in response:
                tools = response.get("result", {}).get("tools")
                if tools:
                    CACHED_TOOLS = tools
                    log_debug(f"Cached {len(tools)} tools from OrcaSlicer")

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

#!/usr/bin/env python3
"""
OrcaMCP Bridge

Translates MCP stdio protocol to HTTP requests to OrcaSlicer's embedded HTTP server.
This allows Claude Code to communicate with OrcaSlicer via the standard MCP interface.

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
    log_debug(f"Starting OrcaMCP Bridge")
    log_debug(f"Connecting to: {ORCAMCP_URL}")

    # Read JSON-RPC messages from stdin (one per line)
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue

        try:
            request = json.loads(line)
        except json.JSONDecodeError as e:
            # Invalid JSON - send error response
            error_response = make_error_response(0, -32700, f"Parse error: {str(e)}")
            print(json.dumps(error_response), flush=True)
            continue

        # Forward request to OrcaSlicer HTTP server
        response = send_request(request)

        # Send response to stdout
        print(json.dumps(response), flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        log_debug("Interrupted")
        sys.exit(0)
    except Exception as e:
        log_debug(f"Fatal error: {e}")
        sys.exit(1)

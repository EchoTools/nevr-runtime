#!/usr/bin/env python3
"""Quick test to start an Echo VR server without DLLs"""

import json
import subprocess
import sys
import time


def send_request(proc, method, params=None):
    """Send JSON-RPC request to MCP server"""
    request = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}}

    request_json = json.dumps(request) + "\n"
    proc.stdin.write(request_json.encode())
    proc.stdin.flush()

    # Read response
    response_line = proc.stdout.readline()
    if not response_line:
        return None

    return json.loads(response_line)


def main():
    # Start MCP server
    mcp_path = "/home/andrew/src/evr-test-harness/bin/evr-mcp"
    proc = subprocess.Popen(
        [mcp_path],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd="/home/andrew/src/evr-test-harness",
    )

    print("MCP server started, PID:", proc.pid)

    try:
        # Initialize MCP connection
        print("\n1. Initializing MCP connection...")
        response = send_request(
            proc,
            "initialize",
            {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "quick-test", "version": "1.0.0"},
            },
        )
        print("   ✓ Initialized")

        # Start Echo VR server
        print("\n2. Starting Echo VR server (no DLLs)...")
        response = send_request(
            proc,
            "tools/call",
            {
                "name": "echovr_start",
                "arguments": {
                    "http_port": 6721,
                    "gametype": "Echo_Arena",
                    "level": "mpl_arena_a",
                    "wait_ready": True,
                    "timeout_seconds": 60,
                    "headless": True,
                },
            },
        )

        if response.get("result", {}).get("isError"):
            content = response["result"]["content"][0]["text"]
            print(f"   ✗ Error: {content}")
            return 1

        # Parse result
        result_text = response["result"]["content"][0]["text"]
        result = json.loads(result_text)

        session_id = result.get("session_id")
        pid = result.get("pid")
        http_port = result.get("http_port")

        print(f"   ✓ Server started!")
        print(f"   - Session ID: {session_id}")
        print(f"   - PID: {pid}")
        print(f"   - HTTP Port: {http_port}")

        # Query state
        print("\n3. Querying server state...")
        response = send_request(
            proc,
            "tools/call",
            {
                "name": "echovr_state",
                "arguments": {"session_id": session_id, "include": ["game_status"]},
            },
        )

        result_text = response["result"]["content"][0]["text"]
        state = json.loads(result_text)

        print(f"   ✓ State: {state.get('game_status', 'unknown')}")

        print(f"\n✅ Echo VR server is running at http://localhost:{http_port}/")
        print(f"\nTo stop: Use session_id '{session_id}' with echovr_stop")
        print(f"\nPress Ctrl+C to stop the server and exit...")

        # Keep running
        while True:
            time.sleep(1)

    except KeyboardInterrupt:
        print("\n\n4. Stopping server...")
        try:
            response = send_request(
                proc,
                "tools/call",
                {"name": "echovr_stop", "arguments": {"session_id": session_id}},
            )
            print("   ✓ Server stopped")
        except:
            pass
    finally:
        proc.terminate()
        proc.wait()
        print("   ✓ MCP server terminated")


if __name__ == "__main__":
    sys.exit(main())

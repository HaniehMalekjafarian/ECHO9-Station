"""M1 Fix 02: drip-fed requests must not hold the sequential server indefinitely.

Runs against the real C server with a test-only Ollama API service.  Does not
require a downloaded model, the dashboard, or extra third-party Python libraries.
"""

import json
import os
import socket
import subprocess
import threading
import time

SOCKET_PATH = os.path.join(os.getenv("AGENTOS_RUNTIME_DIR") or f"/tmp/agentos-{os.geteuid()}", "agentos.sock")


def connect_with_retry(server):
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        client.settimeout(6)
        try:
            client.connect(SOCKET_PATH)
            return client
        except (FileNotFoundError, ConnectionRefusedError):
            client.close()
            if server.poll() is not None:
                raise RuntimeError("AgentOS exited before accepting connections")
            time.sleep(0.02)
    raise RuntimeError("AgentOS socket did not become ready")


def read_json_line(client):
    with client.makefile("rb") as stream:
        raw = stream.readline(16386)
    assert raw.endswith(b"\n"), f"Missing complete response: {raw!r}"
    return json.loads(raw)


def trickle(client, done):
    # A new byte every 200 ms prevents the old 2-second inactivity timeout.
    # Never send '\n': the request cannot complete normally.
    while not done.wait(0.2):
        try:
            client.sendall(b"x")
        except OSError:
            return


def main():
    if os.path.lexists(SOCKET_PATH):
        raise RuntimeError("Stop the previous AgentOS server before running this test")

    env = dict(os.environ, AGENTOS_BACKEND="ollama")
    server = subprocess.Popen(
        ["./build/agentos"], env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    slow = fast = None
    done = threading.Event()
    sender = None
    try:
        slow = connect_with_retry(server)
        slow.sendall(b"{")
        started = time.monotonic()
        sender = threading.Thread(target=trickle, args=(slow, done), daemon=True)
        sender.start()

        # The second client should remain queued until the first request
        # reaches the absolute read deadline, then complete normally.
        time.sleep(0.15)
        fast = connect_with_retry(server)
        request = {
            "version": 1,
            "request_id": "after-slow-client",
            "agent_id": "medical-agent",
            "operation": "infer",
            "payload": {"prompt": "Check reactor"},
        }
        fast.sendall((json.dumps(request) + "\n").encode("utf-8"))
        fast_response = read_json_line(fast)
        slow_response = read_json_line(slow)
        elapsed = time.monotonic() - started

        assert slow_response["status"] == "error", slow_response
        assert slow_response["error"]["code"] == "REQUEST_DEADLINE_EXCEEDED", slow_response
        assert slow_response["request_id"] is None
        assert fast_response["status"] == "ok", fast_response
        assert fast_response["request_id"] == "after-slow-client", fast_response
        assert "Reactor temperature" in fast_response["result"]["text"]
        # Allow some CI scheduling slack around the configured 3s deadline.
        assert 2.3 <= elapsed <= 4.8, f"Unexpected request lifetime {elapsed:.2f}s"
        assert server.poll() is None, "The original server must remain alive"
        print("PASS: trickle-fed client receives REQUEST_DEADLINE_EXCEEDED")
        print("PASS: later valid client progresses on the same server")
        print(f"PASS: request/read delay bounded ({elapsed:.2f} seconds)")
    finally:
        done.set()
        if sender:
            sender.join(timeout=2)
        if fast:
            fast.close()
        if slow:
            slow.close()
        if server.poll() is None:
            server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
    assert not os.path.lexists(SOCKET_PATH), "Server did not clean up its socket"


if __name__ == "__main__":
    main()

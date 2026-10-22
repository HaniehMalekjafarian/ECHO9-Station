
import json
import os
import socket
import subprocess
import time

SOCKET_PATH = os.path.join(os.getenv("AGENTOS_RUNTIME_DIR") or f"/tmp/agentos-{os.geteuid()}", "agentos.sock")


def send_request(request_id=None, prompt=None, raw=None):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(5)

        # Retry briefly if the server is still starting.
        deadline = time.monotonic() + 3

        while True:
            try:
                client.connect(SOCKET_PATH)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.02)

        if raw is None:
            request = {
                "version": 1,
                "request_id": request_id,
                "agent_id": "test-agent",
                "operation": "infer",
                "payload": {"prompt": prompt}
            }
            raw = (json.dumps(request) + "\n").encode()

        client.sendall(raw)

        with client.makefile("rb") as stream:
            response_line = stream.readline(16386)

        assert response_line.endswith(b"\n")
        return json.loads(response_line)


def main():
    if os.path.lexists(SOCKET_PATH):
        raise RuntimeError(
            "Socket already exists. Stop AgentOS before testing."
        )

    server = subprocess.Popen(
        ["./build/agentos"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )

    try:
        # Test 1: First request.
        r1 = send_request("persist-1", "Check oxygen")
        assert r1["status"] == "ok"
        assert r1["request_id"] == "persist-1"
        assert server.poll() is None
        print("PASS: First request")

        # Test 2: Different request, same server PID.
        original_pid = server.pid

        r2 = send_request("persist-2", "Check reactor")
        assert r2["status"] == "ok"
        assert r2["request_id"] == "persist-2"
        assert "Reactor temperature" in r2["result"]["text"]
        assert server.pid == original_pid
        assert server.poll() is None
        print("PASS: Multiple requests, same server")

        # Test 3: A client stalls without sending a newline.
        stalled = send_request(raw=b'{"version":1')
        assert stalled["status"] == "error"
        assert stalled["error"]["code"] == "REQUEST_TIMEOUT"
        print("PASS: Stalled client timeout")

        # Test 4: Server still works after the timeout.
        r3 = send_request("persist-3", "Check oxygen")
        assert r3["status"] == "ok"
        assert r3["request_id"] == "persist-3"
        assert server.poll() is None
        print("PASS: Recovery after stalled client")

        # Test 5: A second server cannot claim the socket.
        second = subprocess.run(
            ["./build/agentos"],
            capture_output=True,
            text=True,
            timeout=3
        )

        assert second.returncode != 0
        assert server.poll() is None

        r4 = send_request("persist-4", "Hello")
        assert r4["status"] == "ok"
        print("PASS: Duplicate server prevented")

        # Test 6: Graceful shutdown.
        server.terminate()
        server.wait(timeout=5)

        assert server.returncode == 0
        assert not os.path.lexists(SOCKET_PATH)
        print("PASS: Graceful shutdown and cleanup")

        print("\nAll 6 persistence tests passed!")

    finally:
        if server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()


if __name__ == "__main__":
    main()


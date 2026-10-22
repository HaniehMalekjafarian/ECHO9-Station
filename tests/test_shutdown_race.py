"""Regression: an idle server must stop promptly on SIGTERM without a client wakeup.

Run with all other AgentOS instances stopped. Tests graceful shutdown under
repeated launches, plus shutdown immediately after serving a real request.
"""
import json
import os
import socket
import subprocess
import time

SOCKET_PATH = os.path.join(os.getenv("AGENTOS_RUNTIME_DIR") or f"/tmp/agentos-{os.geteuid()}", "agentos.sock")


def wait_for_server(process):
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(f"AgentOS died with return code {process.returncode}")
        if os.path.lexists(SOCKET_PATH):
            return
        time.sleep(0.005)
    raise AssertionError("AgentOS did not create socket")


def send_one_request():
    data = {
        "version": 1,
        "request_id": "shutdown-probe",
        "agent_id": "probe-agent",
        "operation": "infer",
        "payload": {"prompt": "Check oxygen"},
    }
    deadline = time.monotonic() + 2
    while True:
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
                s.settimeout(3)
                s.connect(SOCKET_PATH)
                s.sendall((json.dumps(data) + "\n").encode())
                with s.makefile("rb") as stream:
                    response = json.loads(stream.readline(16386))
                assert response["status"] == "ok", response
                assert response["request_id"] == "shutdown-probe", response
                return
        except (ConnectionRefusedError, FileNotFoundError):
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.005)


def run_cycle(index):
    assert not os.path.lexists(SOCKET_PATH), "Stop existing AgentOS first"
    server = subprocess.Popen(
        ["./build/agentos"], stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        env={**os.environ, "AGENTOS_BACKEND": "ollama"},
    )
    try:
        wait_for_server(server)
        if index % 2 == 1:
            send_one_request()
        # Stop while the listener is idle or has just finished a request.
        server.terminate()
        try:
            _, stderr = server.communicate(timeout=2.0)
        except subprocess.TimeoutExpired as exc:
            raise AssertionError("AgentOS did not exit on SIGTERM within 2s") from exc
        assert server.returncode == 0, (server.returncode, stderr.decode())
        assert not os.path.lexists(SOCKET_PATH), "Socket not removed on shutdown"
    finally:
        if server.poll() is None:
            server.kill()
            server.communicate(timeout=3)


def main():
    for i in range(100):
        run_cycle(i)
    print("PASS: 100 repeated idle/after-request SIGTERM shutdowns")
    print("PASS: server exits successfully and removes socket each time")


if __name__ == "__main__":
    main()

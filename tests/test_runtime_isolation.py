#!/usr/bin/env python3
"""Fix 04: runtime isolation, crash recovery, and unsafe-path rejection.

The tests use independent private directories under a temporary directory.
They do not touch the default AgentOS socket or a running instructor demo.
"""

import json
import os
from pathlib import Path
import socket
import stat
import subprocess
import tempfile
import urllib.request
import time

BINARY = "./build/agentos"


def child_env(runtime_dir):
    return {**os.environ, "AGENTOS_BACKEND": "ollama",
            "AGENTOS_RUNTIME_DIR": str(runtime_dir)}


def launch(directory):
    child = subprocess.Popen(
        [BINARY], env=child_env(directory),
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
    )
    path = directory / "agentos.sock"
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if child.poll() is not None:
            err = child.stderr.read().decode(errors="replace")
            raise AssertionError(f"Server exited early: {child.returncode}: {err}")
        if path.is_socket():
            # After SIGKILL a stale socket path already exists; only a
            # successful connect proves that the replacement is listening.
            try:
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
                    probe.settimeout(0.1)
                    probe.connect(str(path))
                return child
            except (ConnectionRefusedError, FileNotFoundError, TimeoutError):
                pass
        time.sleep(0.01)
    stop(child)
    raise AssertionError("Server socket was not created")


def stop(child, *, kill=False):
    if child.poll() is None:
        if kill:
            child.kill()
        else:
            child.terminate()
        try:
            child.communicate(timeout=4)
        except subprocess.TimeoutExpired:
            child.kill()
            child.communicate(timeout=3)
            raise AssertionError("AgentOS did not stop promptly")
    else:
        child.communicate(timeout=3)


def request(directory, request_id):
    msg = json.dumps({
        "version": 1, "request_id": request_id,
        "agent_id": "isolation-check", "operation": "infer",
        "payload": {"prompt": "Check oxygen"},
    }).encode() + b"\n"
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(4)
        client.connect(str(directory / "agentos.sock"))
        client.sendall(msg)
        with client.makefile("rb") as stream:
            response = json.loads(stream.readline(16386))
    assert response["status"] == "ok", response
    assert response["request_id"] == request_id, response


def check_ui_bridge(directory):
    # Verify that the real Mission Control selects the same socket as C,
    # without relying on the old AGENTOS_SOCKET compatibility override.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]

    env = child_env(directory)
    env.pop("AGENTOS_SOCKET", None)
    env.update(AGENTOS_UI_PORT=str(port), AGENTOS_UI_BIND="127.0.0.1")
    bridge = subprocess.Popen(
        ["python3", "demo/mission_control.py"], env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
    )
    try:
        deadline = time.monotonic() + 12
        url = f"http://127.0.0.1:{port}"
        while True:
            if bridge.poll() is not None:
                raise AssertionError("Mission Control exited unexpectedly")
            try:
                with urllib.request.urlopen(url + "/api/status", timeout=0.5) as r:
                    status = json.load(r)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise AssertionError("Mission Control did not start within 12 seconds")
                time.sleep(0.02)
        assert status["socket_present"], status
        body = json.dumps({"agent": "medical", "prompt": "Check oxygen"}).encode()
        req = urllib.request.Request(url + "/api/request", data=body,
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=5) as r:
            result = json.load(r)
        assert result["response"]["status"] == "ok", result
        assert result["response"]["request_id"] == result["id"], result
    finally:
        stop(bridge)


def rejected(directory):
    proc = subprocess.run(
        [BINARY], env=child_env(directory),
        capture_output=True, text=True, timeout=3,
    )
    assert proc.returncode != 0, "Unsafe/duplicate AgentOS unexpectedly started"
    return proc.stderr


def main():
    with tempfile.TemporaryDirectory(prefix="agentos-isolation-") as parent:
        root = Path(parent)
        a = root / "project-a"
        b = root / "project-b"
        child_a = child_b = None
        try:
            child_a = launch(a)
            child_b = launch(b)
            assert a.stat().st_uid == os.geteuid()
            assert b.stat().st_uid == os.geteuid()
            assert stat.S_IMODE(a.stat().st_mode) == 0o700
            assert stat.S_IMODE(b.stat().st_mode) == 0o700
            assert stat.S_IMODE((a / "agentos.sock").stat().st_mode) & 0o077 == 0
            request(a, "alpha-1")
            request(b, "bravo-1")
            print("PASS: independent servers serve requests in private 0700 directories")

            check_ui_bridge(a)
            print("PASS: Mission Control forwards to the selected runtime directory")

            assert "already running" in rejected(a).lower()
            request(a, "alpha-2")
            request(b, "bravo-2")
            print("PASS: duplicate instance cannot damage another live instance")

            stop(child_a)
            child_a = None
            assert not (a / "agentos.sock").exists()
            request(b, "bravo-3")
            print("PASS: stopping A removes only A's socket, B continues")

            child_a = launch(a)
            stop(child_a, kill=True)
            child_a = None
            assert (a / "agentos.sock").is_socket(), "SIGKILL must leave stale socket"
            child_a = launch(a)
            request(a, "restarted-alpha")
            request(b, "still-bravo")
            print("PASS: SIGKILL recovery works independently of other instances")
        finally:
            for child in [child_a, child_b]:
                if child is not None:
                    stop(child)

        unsafe = root / "world-accessible"
        unsafe.mkdir(mode=0o755)
        unsafe.chmod(0o755)
        assert "mode 0700" in rejected(unsafe)
        assert not (unsafe / "agentos.sock").exists()
        print("PASS: insecure runtime-directory permissions rejected")

        target = root / "real-dir"
        target.mkdir(mode=0o700)
        link = root / "symlink-dir"
        link.symlink_to(target, target_is_directory=True)
        rejected(link)
        assert not (target / "agentos.sock").exists()
        print("PASS: symlink runtime directory rejected")

        poisoned = root / "regular-file-socket"
        poisoned.mkdir(mode=0o700)
        (poisoned / "agentos.sock").write_text("do-not-delete")
        rejected(poisoned)
        assert (poisoned / "agentos.sock").read_text() == "do-not-delete"
        print("PASS: non-socket path is never removed")

        assert "absolute" in rejected(Path("relative/path"))
        print("PASS: relative runtime path rejected")

    print("\nAll 9 runtime isolation checks passed!")


if __name__ == "__main__":
    main()

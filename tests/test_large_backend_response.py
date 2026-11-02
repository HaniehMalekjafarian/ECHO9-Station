"""Regression for escaped responses growing beyond AgentOS's framed JSON limit.

Uses a local fake Ollama endpoint; no real model or network download required.
Run while any manually launched AgentOS instance is stopped.
"""
import json
import os
import socket
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SOCKET_PATH = os.path.join(os.getenv("AGENTOS_RUNTIME_DIR") or f"/tmp/agentos-{os.geteuid()}", "agentos.sock")


class FakeModel(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        if self.path != "/api/generate":
            self.send_error(404)
            return
        length = int(self.headers["Content-Length"])
        req = json.loads(self.rfile.read(length))
        if req["prompt"] == "overflow":
            # Fits in the raw 4096-byte C backend buffer, but escaped JSON
            # expands to > 16 KiB because json-c writes \u0001 for each byte.
            answer = "\x01" * 3900
        elif req["prompt"] == "long-plain":
            answer = "a" * 3900
        else:
            answer = "Model recovered"
        raw = json.dumps({"response": answer}).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)


def request(identifier, prompt):
    data = {"version": 1, "request_id": identifier, "agent_id": "research",
            "operation": "infer", "payload": {"prompt": prompt}}
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(6)
        client.connect(SOCKET_PATH)
        client.sendall((json.dumps(data) + "\n").encode("utf-8"))
        with client.makefile("rb") as stream:
            response_line = stream.readline(16386)
        assert response_line.endswith(b"\n"), (
            "AgentOS closed the connection without a framed JSON response"
        )
        assert len(response_line) - 1 <= 16384, "Response exceeds protocol limit"
        return json.loads(response_line)


def main():
    if os.path.lexists(SOCKET_PATH):
        raise RuntimeError("Stop any existing AgentOS before running this test")
    fake = ThreadingHTTPServer(("127.0.0.1", 0), FakeModel)
    worker = threading.Thread(target=fake.serve_forever, daemon=True)
    worker.start()
    env = os.environ.copy()
    env.update(AGENTOS_BACKEND="ollama", OLLAMA_MODEL="fake-model",
               OLLAMA_URL=f"http://127.0.0.1:{fake.server_port}/api/generate")
    server = subprocess.Popen(["./build/agentos"], env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 4
        while not os.path.exists(SOCKET_PATH):
            if server.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("AgentOS did not start")
            time.sleep(.02)

        r1 = request("oversize-escape-01", "overflow")
        assert r1["request_id"] == "oversize-escape-01", r1
        assert r1["status"] == "error", r1
        assert r1["error"]["code"] == "RESPONSE_TOO_LARGE", r1
        print("PASS: oversized JSON-escaped model reply returns structured error")

        r2 = request("plain-02", "long-plain")
        assert r2["status"] == "ok", r2
        assert r2["request_id"] == "plain-02", r2
        assert r2["result"]["text"] == "a" * 3900, r2
        print("PASS: valid large model reply still succeeds")

        r3 = request("recovered-03", "short")
        assert r3["status"] == "ok", r3
        assert r3["request_id"] == "recovered-03", r3
        assert r3["result"]["text"] == "Model recovered", r3
        assert server.poll() is None
        print("PASS: same AgentOS process continues after overflow")
        print("\nAll 3 large-response checks passed!")
    finally:
        if server.poll() is None:
            server.terminate()
        try:
            server.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.communicate(timeout=5)
        fake.shutdown()
        fake.server_close()


if __name__ == "__main__":
    main()

"""End-to-end smoke test for AgentOS -> HTTP model adapter using a fake Ollama.
No model installation or download required. Run with the AgentOS server stopped.
"""
import json
import os
import socket
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SOCKET = os.path.join(os.getenv("AGENTOS_RUNTIME_DIR") or f"/tmp/agentos-{os.geteuid()}", "agentos.sock")

class FakeModel(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        assert self.path == "/api/generate", self.path
        length = int(self.headers["Content-Length"])
        request = json.loads(self.rfile.read(length))
        assert request["stream"] is False
        assert request["model"] == "test-model"
        response = {"response": "FAKE-LM ANSWER: " + request["prompt"]}
        body = json.dumps(response).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    if os.path.lexists(SOCKET):
        raise RuntimeError("Stop an existing AgentOS and clear stale sockets safely first")
    httpd = ThreadingHTTPServer(("127.0.0.1", 0), FakeModel)
    runner = threading.Thread(target=httpd.serve_forever, daemon=True)
    runner.start()
    env = os.environ.copy()
    env.update(AGENTOS_BACKEND="ollama", OLLAMA_MODEL="test-model",
               OLLAMA_URL=f"http://127.0.0.1:{httpd.server_port}/api/generate")
    server = subprocess.Popen(["./build/agentos"], env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic()+4
        while not os.path.exists(SOCKET):
            if time.monotonic()>deadline or server.poll() is not None:
                raise RuntimeError("AgentOS did not start")
            time.sleep(.03)
        request={"version":1,"request_id":"ollama-smoke-1", "agent_id":"test-agent",
                 "operation":"infer", "payload":{"prompt":"Hello, ECHO-9!"}}
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(5)
            client.connect(SOCKET)
            client.sendall((json.dumps(request)+'\n').encode())
            with client.makefile('rb') as stream:
                response=json.loads(stream.readline())
        assert response["status"]=="ok", response
        assert response["request_id"]==request["request_id"]
        assert response["result"]["text"]=="FAKE-LM ANSWER: Hello, ECHO-9!", response
        print("PASS: C AgentOS -> HTTP Ollama adapter -> C AgentOS -> Python client")
    finally:
        server.terminate()
        try: server.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill();server.communicate()
        httpd.shutdown();httpd.server_close()

if __name__=='__main__':
    main()

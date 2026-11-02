"""Deterministic Ollama /api/generate test fixture (NOT a student inference backend).
Used only by make test to make network transport tests reproducible.
"""
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

class TestOllama(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        if self.path != "/api/generate":
            self.send_error(404)
            return
        try:
            count = int(self.headers.get("Content-Length", "0"))
            if count < 1 or count > 100_000: raise ValueError("size")
            payload = json.loads(self.rfile.read(count))
            assert payload["stream"] is False
            prompt = payload["prompt"]
            if not isinstance(prompt, str): raise ValueError("prompt")
        except (ValueError, AssertionError, KeyError, TypeError):
            self.send_error(400)
            return
        if prompt == "__FAIL_BACKEND__":
            self.send_error(503)
            return
        if "oxygen" in prompt:
            answer = "TEST OLLAMA: Oxygen levels are normal."
        elif "reactor" in prompt:
            answer = "TEST OLLAMA: Reactor temperature is stable."
        else:
            answer = "TEST OLLAMA: Received prompt: " + prompt
        raw = json.dumps({"response": answer}, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

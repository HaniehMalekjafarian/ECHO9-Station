#!/usr/bin/env python3
"""Instructor-only UI bridge: browser HTTP -> Unix socket -> C AgentOS.

No pip dependencies. Dashboard events are OBSERVATIONS AT THE BRIDGE,
not internal scheduler/worker metrics. Do not expose on the public internet.
"""
import json
import os
import socket
import stat
import threading
import time
import uuid
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

RUNTIME_DIR = os.getenv("AGENTOS_RUNTIME_DIR") or f"/tmp/agentos-{os.geteuid()}"
# The explicit socket override remains for standalone UI bridge testing.
SOCKET_PATH = os.getenv("AGENTOS_SOCKET") or os.path.join(RUNTIME_DIR, "agentos.sock")
PORT = int(os.getenv("AGENTOS_UI_PORT", "8765"))
BIND = os.getenv("AGENTOS_UI_BIND", "127.0.0.1")
HTML = Path(__file__).with_name("index.html").read_bytes()
STATION_HTML = Path(__file__).with_name("station.html").read_bytes()
WORKBENCH_HTML = Path(__file__).with_name("workbench.html").read_bytes()
PROCESSES_HTML = Path(__file__).with_name("processes.html").read_bytes()
CONTROL_SOCKET = os.getenv("AGENTOS_CONTROL_SOCKET") or os.path.join(RUNTIME_DIR, "control.sock")
AGENTS = {"medical", "maintenance", "research", "navigation"}
EVENTS = deque(maxlen=100)
LOCK = threading.Lock()
STARTED = time.monotonic()


def log_event(kind, rid, agent, detail):
    with LOCK:
        EVENTS.appendleft({
            "ts": time.strftime("%H:%M:%S"), "at_ms": int(time.time() * 1000), "kind": kind,
            "id": rid, "agent": agent, "detail": detail,
        })


def socket_present():
    try:
        return stat.S_ISSOCK(os.stat(SOCKET_PATH).st_mode)
    except OSError:
        return False


def send_agent_request(agent, prompt, rid):
    request = {"version": 1, "request_id": rid,
               "agent_id": agent + "-agent", "operation": "infer",
               "payload": {"prompt": prompt}}
    data = (json.dumps(request, ensure_ascii=False) + "\n").encode("utf-8")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        # Real model inference may take much longer than the 2s receive-idle
        # timeout on AgentOS's socket: that timeout applies per recv call.
        client.settimeout(125)
        client.connect(SOCKET_PATH)
        client.sendall(data)
        with client.makefile("rb") as stream:
            response_data = stream.readline(16386)
        if not response_data.endswith(b"\n"):
            raise ValueError("AgentOS returned an incomplete response")
        response = json.loads(response_data)
        if response.get("request_id") != rid:
            raise ValueError("AgentOS returned the wrong request ID")
        return response



def send_control(operation, **fields):
    payload = {"version": 1, "operation": operation, **fields}
    # Prevent a timed-out browser request from performing a stale action later
    # after the synchronous P1 inference has finally returned.
    payload["expires_monotonic_ms"] = int(time.monotonic() * 1000) + 8500
    raw = (json.dumps(payload, ensure_ascii=True) + "\n").encode("ascii")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(10)
        client.connect(CONTROL_SOCKET)
        client.sendall(raw)
        with client.makefile("rb") as stream:
            line = stream.readline(300000)
    if not line.endswith(b"\n"):
        raise ValueError("Incomplete local-control response")
    return json.loads(line)


def permitted_local_host(host):
    if not host:
        return False
    hostname = urlsplit("http://" + host).hostname
    return hostname in ("localhost", "127.0.0.1", "[::1]", "::1")

class Handler(BaseHTTPRequestHandler):
    server_version = "AgentOS-MissionControl/0.1"

    def log_message(self, fmt, *args):
        pass

    def json_response(self, code, data):
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(HTML)))
            self.end_headers()
            self.wfile.write(HTML)
        elif self.path in ("/station", "/station.html"):
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(STATION_HTML)))
            self.end_headers()
            self.wfile.write(STATION_HTML)
        elif self.path in ("/workbench", "/workbench.html"):
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(WORKBENCH_HTML)))
            self.end_headers()
            self.wfile.write(WORKBENCH_HTML)
        elif self.path in ("/processes", "/processes.html"):
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(PROCESSES_HTML)))
            self.end_headers()
            self.wfile.write(PROCESSES_HTML)
        elif self.path.split("?", 1)[0] in ("/api/processes", "/api/process-events"):
            if BIND not in ("127.0.0.1", "localhost", "::1") or not permitted_local_host(self.headers.get("Host")):
                return self.json_response(403, {"error": "Control monitor restricted to local host"})
            route, _, query = self.path.partition("?")
            try:
                if route == "/api/processes":
                    data = send_control("list")
                else:
                    from urllib.parse import parse_qs
                    since = int(parse_qs(query).get("since", ["0"])[0])
                    if since < 0:
                        raise ValueError("negative sequence")
                    data = send_control("events", since=since)
                return self.json_response(200, data)
            except (OSError, ValueError, TimeoutError) as exc:
                return self.json_response(503, {"error": str(exc)[:160]})
        elif self.path == "/api/status":
            with LOCK:
                events = list(EVENTS)
            self.json_response(200, {
                "socket_present": socket_present(),
                "backend": "ollama",
                "model": os.getenv("OLLAMA_MODEL", "qwen2.5:0.5b"),
                "events": events,
                "uptime_s": round(time.monotonic() - STARTED),
            })
        else:
            self.json_response(404, {"error": "Not found"})

    def do_POST(self):
        if self.path not in ("/api/request", "/api/workbench", "/api/process-action", "/api/agent-infer"):
            return self.json_response(404, {"error": "Not found"})

        if self.path in ("/api/process-action", "/api/agent-infer"):
            if BIND not in ("127.0.0.1", "localhost", "::1") or not permitted_local_host(self.headers.get("Host")):
                return self.json_response(403, {"error": "Local management only"})
            # Non-simple custom header plus Origin checking defend against
            # cross-site browser form/JSON triggers. Not a remote admin API.
            if self.headers.get("X-AgentOS-Control") != "1":
                return self.json_response(403, {"error": "Missing local control header"})
            if not self.headers.get("Origin"):
                return self.json_response(403, {"error": "Origin required for management"})
        # This local instructor UI accepts JSON only. Requiring JSON also
        # prevents cross-origin browsers from sending a simple-form POST.
        if self.headers.get_content_type() != "application/json":
            return self.json_response(415, {"error": "JSON Content-Type required"})

        # A same-origin browser request is allowed; third-party web pages
        # must not silently trigger local model inference.
        origin = self.headers.get("Origin")
        if origin is not None:
            parsed = urlsplit(origin)
            host = self.headers.get("Host", "")
            if (parsed.scheme not in ("http", "https") or
                    not host or parsed.netloc.lower() != host.lower()):
                return self.json_response(403, {"error": "Cross-origin request denied"})

        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > (35000 if self.path == "/api/workbench" else 8192):
                return self.json_response(413, {"error": "Invalid request size"})
            payload = json.loads(self.rfile.read(length))
            if self.path == "/api/workbench":
                return self.handle_workbench(payload)
            if self.path == "/api/process-action":
                return self.handle_process_action(payload)
            if self.path == "/api/agent-infer":
                return self.handle_agent_infer(payload)
            agent = payload.get("agent")
            prompt = payload.get("prompt")
            if agent not in AGENTS or not isinstance(prompt, str) or not (1 <= len(prompt) <= 1600):
                return self.json_response(400, {"error": "Invalid agent or prompt"})
        except (ValueError, TypeError, AttributeError, json.JSONDecodeError):
            return self.json_response(400, {"error": "Malformed request"})
        rid = "demo-" + uuid.uuid4().hex[:10]
        log_event("ARRIVED", rid, agent, "Request submitted to AgentOS")
        started = time.monotonic()
        try:
            response = send_agent_request(agent, prompt, rid)
        except (OSError, ValueError, TimeoutError) as exc:
            log_event("TRANSPORT_ERROR", rid, agent, str(exc)[:120])
            return self.json_response(502, {"id": rid, "error": str(exc)})
        elapsed = round((time.monotonic() - started) * 1000)
        ok = response.get("status") == "ok"
        log_event("COMPLETED" if ok else "FAILED", rid, agent,
                  f"Round-trip {elapsed} ms")
        self.json_response(200, {"id": rid, "agent": agent, "response": response, "latency_ms": elapsed})


    def handle_process_action(self, payload):
        if not isinstance(payload, dict):
            return self.json_response(400, {"error": "Invalid action"})
        op = payload.get("operation")
        if op not in ("add", "terminate", "reap", "remove", "reactivate", "wait"):
            return self.json_response(400, {"error": "Unsupported action"})
        fields = {}
        if op in ("add", "remove", "reactivate"):
            agent_id = payload.get("agent_id")
            if not isinstance(agent_id, str) or not 1 <= len(agent_id.encode("utf-8")) <= 128:
                return self.json_response(400, {"error": "Invalid agent identity"})
            fields["agent_id"] = agent_id
            if op in ("add", "reactivate"):
                role = payload.get("role", "GENERAL")
                if not isinstance(role, str) or len(role.encode("utf-8")) > 32:
                    return self.json_response(400, {"error": "Invalid role"})
                fields["role"] = role
        else:
            pid = payload.get("pid")
            if type(pid) is not int or not 0 < pid < (1 << 63):
                return self.json_response(400, {"error": "Invalid PID"})
            fields["pid"] = pid
        try:
            result = send_control(op, **fields)
            return self.json_response(200 if result.get("status") == "ok" else 409, result)
        except (OSError, ValueError, TimeoutError) as exc:
            return self.json_response(503, {"error": "Action outcome not confirmed; refresh roster: " + str(exc)[:110]})

    def handle_agent_infer(self, payload):
        if not isinstance(payload, dict):
            return self.json_response(400, {"error": "Invalid inference"})
        agent = payload.get("agent_id")
        prompt = payload.get("prompt")
        if (not isinstance(agent, str) or not 1 <= len(agent.encode("utf-8")) <= 128 or
                not isinstance(prompt, str) or not 1 <= len(prompt.encode("utf-8")) <= 2048):
            return self.json_response(400, {"error": "Invalid agent or prompt"})
        rid = "process-" + uuid.uuid4().hex[:10]
        request = {"version": 1, "request_id": rid, "agent_id": agent,
                   "operation": "infer", "payload": {"prompt": prompt}}
        raw = (json.dumps(request, ensure_ascii=False) + "\n").encode("utf-8")
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
                sock.settimeout(125)
                sock.connect(SOCKET_PATH)
                sock.sendall(raw)
                with sock.makefile("rb") as stream:
                    line = stream.readline(16386)
            if not line.endswith(b"\n"):
                raise ValueError("Incomplete AgentOS response")
            return self.json_response(200, {"response": json.loads(line), "agent_id": agent})
        except (OSError, TimeoutError, ValueError) as exc:
            return self.json_response(502, {"error": str(exc)[:140]})

    def handle_workbench(self, payload):
        # Bounded raw-protocol experiments to the same fixed local Unix socket.
        # No arbitrary file paths or external endpoints can be supplied.
        if not isinstance(payload, dict):
            return self.json_response(400, {"error": "Expected JSON object"})
        frame = payload.get("frame")
        add_newline = payload.get("append_newline", True)
        fragmented = payload.get("fragment_size", 0)
        halfclose = payload.get("halfclose", False)
        if (not isinstance(frame, str) or not isinstance(add_newline, bool)
                or type(fragmented) is not int or fragmented not in (0, 1, 5)
                or not isinstance(halfclose, bool)):
            return self.json_response(400, {"error": "Invalid workbench request"})
        raw = frame.encode("utf-8")
        if len(raw) > 20000:
            return self.json_response(413, {"error": "Workbench input exceeds 20000 bytes"})
        if add_newline:
            raw += b"\n"
        started = time.monotonic()
        line = b""
        transport_error = None
        parsed = None
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(125)
                client.connect(SOCKET_PATH)
                if fragmented:
                    for i in range(0, len(raw), fragmented):
                        client.sendall(raw[i:i + fragmented])
                else:
                    client.sendall(raw)
                if halfclose:
                    client.shutdown(socket.SHUT_WR)
                with client.makefile("rb") as stream:
                    line = stream.readline(16386)
            if not line.endswith(b"\n"):
                transport_error = "No complete newline-terminated response"
            else:
                try:
                    parsed = json.loads(line)
                except (UnicodeDecodeError, ValueError):
                    transport_error = "Server replied with invalid JSON"
        except (OSError, TimeoutError, ValueError) as exc:
            transport_error = f"{type(exc).__name__}: {str(exc)[:160]}"
        elapsed = round((time.monotonic() - started) * 1000)
        self.json_response(200, {
            "raw_response": line.decode("utf-8", errors="replace"),
            "response": parsed,
            "transport_error": transport_error,
            "bytes_sent": len(raw),
            "duration_ms": elapsed,
        })


if __name__ == "__main__":
    server = ThreadingHTTPServer((BIND, PORT), Handler)
    print(f"AgentOS Mission Control: http://localhost:{PORT}", flush=True)
    print("Dashboard events reflect the UI bridge, not kernel or internal scheduler telemetry.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()

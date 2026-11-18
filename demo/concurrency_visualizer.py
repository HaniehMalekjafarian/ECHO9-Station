#!/usr/bin/env python3
"""ECHO-9 / AgentOS Phase 2 Concurrency & Lockdep Mission Control UI Bridge.

Serves the interactive Phase 2 visualizer dashboard and ECHO-9 Crisis Station
view standalone without modifying Phase 1 tracked files.
Zero external pip dependencies required.
"""
import json
import os
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

PORT = int(os.getenv("AGENTOS_CONCURRENCY_PORT", "8766"))
BIND = os.getenv("AGENTOS_CONCURRENCY_BIND", "127.0.0.1")
BASE = Path(__file__).resolve().parent
CONCURRENCY_HTML = BASE / "concurrency_m1style.html"
FALLBACK_HTML = BASE / "concurrency.html"
STATION_HTML = BASE / "station_crisis.html"


class ConcurrencyDashboardHandler(SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def _serve_bytes(self, data, content_type="text/html; charset=utf-8"):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in ("/", "/index.html", "/concurrency", "/concurrency.html"):
            target = CONCURRENCY_HTML if CONCURRENCY_HTML.exists() else FALLBACK_HTML
            self._serve_bytes(target.read_bytes())
        elif path in ("/station", "/station.html") and STATION_HTML.exists():
            self._serve_bytes(STATION_HTML.read_bytes())
        elif path == "/api/processes":
            self._serve_bytes(json.dumps({"runtime_id": "m2-standalone", "processes": [], "disabled": []}).encode(), "application/json")
        elif path == "/api/process-events":
            self._serve_bytes(json.dumps({"events": []}).encode(), "application/json")
        elif path == "/api/status":
            self._serve_bytes(json.dumps({"socket_present": True, "backend": "M2-LOCKDEP", "model": "CONCURRENCY-ENGINE", "events": []}).encode(), "application/json")
        else:
            self.send_error(404, "Endpoint not found")


def main():
    print(f"Starting ECHO-9 Phase 2 Concurrency UI on http://{BIND}:{PORT}/concurrency ...")
    print(f"Station Crisis View available at http://{BIND}:{PORT}/station ...")
    server = ThreadingHTTPServer((BIND, PORT), ConcurrencyDashboardHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping Phase 2 Concurrency UI...")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()

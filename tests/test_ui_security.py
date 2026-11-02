"""No-dependency regression checks for the M1 demonstration HTTP boundary."""
import importlib.util
import os
import threading
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path

# The default must be safe even without environment configuration.
os.environ.pop("AGENTOS_UI_BIND", None)
source = Path(__file__).resolve().parents[1] / "demo" / "mission_control.py"
spec = importlib.util.spec_from_file_location("agentos_ui_security_test", source)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
assert module.BIND == "127.0.0.1", module.BIND

server = ThreadingHTTPServer(("127.0.0.1", 0), module.Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
base = f"http://127.0.0.1:{server.server_port}"

def post(headers):
    request = urllib.request.Request(base + "/api/request",
        data=b'{"agent":"medical","prompt":"check oxygen"}', headers=headers)
    try:
        urllib.request.urlopen(request, timeout=3)
        raise AssertionError("Expected rejection")
    except urllib.error.HTTPError as err:
        return err.code

try:
    assert urllib.request.urlopen(base + "/", timeout=3).status == 200
    assert b"Protocol Workbench" in urllib.request.urlopen(base + "/workbench", timeout=3).read()
    assert post({"Content-Type":"text/plain"}) == 415
    assert post({"Content-Type":"application/json",
                 "Origin":"https://unrelated.example"}) == 403
    print("PASS: Workbench HTML served on /workbench")
    print("PASS: loopback-only default")
    print("PASS: non-JSON POST rejected")
    print("PASS: cross-origin JSON POST rejected")
finally:
    server.shutdown()
    server.server_close()
    thread.join(timeout=3)

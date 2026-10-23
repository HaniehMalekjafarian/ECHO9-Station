#!/usr/bin/env python3
"""One-command M1 regression suite; isolates tests from the normal demo.

Runs suites sequentially and fails immediately on the first failing suite.
"""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import threading
from ollama_fixture import TestOllama
from http.server import ThreadingHTTPServer

ROOT = Path(__file__).resolve().parents[1]
SUITES = (
    "tests/test_build_dependencies.py",
    "tests/test_process_manager_unit.py",
    "tests/test_process_integration.py",
    "tests/test_section_ui.py",
    "tests/test_m1.py",
    "tests/test_m1_persistence.py",
    "tests/test_slow_client_deadline.py",
    "tests/test_large_backend_response.py",
    "tests/test_shutdown_race.py",
    "tests/test_runtime_isolation.py",
    "tests/test_demo_adapter.py",
    "tests/test_ui_security.py",
    "echo9_station_addon/tests/test_station_addon.py",
)


def main():
    if not (ROOT / "build" / "agentos").is_file():
        print("Missing build/agentos; run 'make' first.", file=sys.stderr)
        return 2

    start = time.monotonic()
    fixture = ThreadingHTTPServer(("127.0.0.1", 0), TestOllama)
    fixture_thread = threading.Thread(target=fixture.serve_forever, daemon=True)
    fixture_thread.start()
    try:
        return run_tests(fixture, start)
    finally:
        fixture.shutdown()
        fixture.server_close()
        fixture_thread.join(timeout=3)


def run_tests(fixture, start):
    with tempfile.TemporaryDirectory(prefix="agentos-m1-tests-") as parent:
        # Create one runtime per full-suite invocation; no collision with
        # the default /tmp/agentos-UID used by a running instructor demo.
        runtime = str(Path(parent) / "runtime")
        env = os.environ.copy()
        env["AGENTOS_RUNTIME_DIR"] = runtime
        env["AGENTOS_BACKEND"] = "ollama"
        env["OLLAMA_URL"] = f"http://127.0.0.1:{fixture.server_port}/api/generate"
        env["OLLAMA_MODEL"] = "test-fixture-model"
        env.pop("AGENTOS_SOCKET", None)

        for index, relative_path in enumerate(SUITES, 1):
            print(f"\n[{index}/{len(SUITES)}] {relative_path}", flush=True)
            try:
                completed = subprocess.run(
                    [sys.executable, "-u", relative_path],
                    cwd=ROOT, env=env, timeout=120, check=False,
                )
            except subprocess.TimeoutExpired:
                print(f"FAIL: {relative_path} exceeded 120 seconds", file=sys.stderr)
                return 1
            if completed.returncode != 0:
                print(
                    f"FAIL: {relative_path} exited with {completed.returncode}",
                    file=sys.stderr,
                )
                return 1

    duration = time.monotonic() - start
    print(f"\nPASS: all {len(SUITES)} AgentOS M1 test suites ({duration:.1f}s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

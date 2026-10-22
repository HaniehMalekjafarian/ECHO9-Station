#!/usr/bin/env python3
"""Exercise the running C AgentOS with a real Ollama inference request.

Start the demo first: bash demo/run_demo.sh ollama
No expected model wording is hard-coded: local models are nondeterministic.
"""
import json
import os
import socket
import sys
import uuid

runtime_dir = os.getenv("AGENTOS_RUNTIME_DIR") or f"/tmp/agentos-{os.geteuid()}"
socket_path = os.path.join(runtime_dir, "agentos.sock")
request_id = "manual-" + uuid.uuid4().hex[:12]
request = {
    "version": 1,
    "request_id": request_id,
    "agent_id": "medical-agent",
    "operation": "infer",
    "payload": {"prompt": "Briefly describe one ECHO-9 station safety check."},
}

try:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(125)  # Inference may take longer than a network read.
        client.connect(socket_path)
        client.sendall((json.dumps(request, ensure_ascii=True) + "\n").encode("utf-8"))
        with client.makefile("rb") as stream:
            response_bytes = stream.readline(16386)

    if not response_bytes.endswith(b"\n"):
        raise RuntimeError("Response is incomplete (missing newline delimiter)")
    response = json.loads(response_bytes)
    if response.get("version") != 1 or response.get("request_id") != request_id:
        raise RuntimeError("AgentOS returned an invalid version or request ID")
    if response.get("status") != "ok":
        detail = response.get("error", {})
        raise RuntimeError(f"AgentOS error: {detail.get('code', 'UNKNOWN')}: {detail.get('message', '')}")
    text = response.get("result", {}).get("text")
    if not isinstance(text, str) or not text.strip():
        raise RuntimeError("AgentOS returned no usable model text")
    print("PASS: real Ollama response received through C AgentOS")
    print(json.dumps(response, indent=2, ensure_ascii=False))
except (OSError, ValueError, RuntimeError) as exc:
    print(f"FAIL: {exc}", file=sys.stderr)
    sys.exit(1)

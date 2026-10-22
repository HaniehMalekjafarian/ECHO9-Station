#!/usr/bin/env python3
"""Local AgentOS operator CLI (control.sock), not the public infer protocol."""
import argparse
import json
import os
import socket
import sys
import time
from pathlib import Path

parser=argparse.ArgumentParser(description="Manage ECHO-9 logical agents (trusted local runtime only)")
parser.add_argument("operation",choices=["list","events","add","remove","reactivate","terminate","wait","reap","inspect"])
parser.add_argument("--agent-id",default="",help="Exact agent identifier for name-based operations")
parser.add_argument("--role",default="GENERAL")
parser.add_argument("--pid",type=int)
parser.add_argument("--since",type=int,default=0)
args=parser.parse_args()
base=os.getenv("AGENTOS_RUNTIME_DIR") or f"/tmp/agentos-{os.geteuid()}"
path=Path(os.getenv("AGENTOS_CONTROL_SOCKET") or str(Path(base)/"control.sock"))
payload={"version":1,"operation":args.operation,
         "expires_monotonic_ms":int(time.monotonic()*1000)+8500}
if args.operation in ("add","remove","reactivate","inspect"):
    if not args.agent_id:parser.error("this operation requires --agent-id")
    payload["agent_id"]=args.agent_id
if args.operation in ("add","reactivate"):
    payload["role"]=args.role
if args.operation in ("terminate","wait","reap"):
    if args.pid is None or args.pid<=0:parser.error("this operation requires --pid > 0")
    payload["pid"]=args.pid
if args.operation=="events":payload["since"]=args.since
try:
    with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as conn:
        conn.settimeout(10)
        conn.connect(str(path))
        conn.sendall((json.dumps(payload,ensure_ascii=True)+"\n").encode("ascii"))
        with conn.makefile("rb") as f:reply=f.readline(300000)
    if not reply.endswith(b"\n"):raise RuntimeError("Incomplete control response")
    data=json.loads(reply)
except (OSError,ValueError,RuntimeError) as exc:
    print(f"Control request not confirmed: {exc}",file=sys.stderr);sys.exit(2)
print(json.dumps(data,indent=2,ensure_ascii=False))
sys.exit(0 if data.get("status")=="ok" else 1)

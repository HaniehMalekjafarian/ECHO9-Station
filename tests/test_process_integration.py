#!/usr/bin/env python3
"""Independent M1+Process integration. Fixture is provided by run_all.py."""
import json
import os
import signal
import socket
import subprocess
import sys
import time
import threading
from http.server import ThreadingHTTPServer
from pathlib import Path
from urllib.request import Request, urlopen
from urllib.error import HTTPError

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = Path(os.environ["AGENTOS_RUNTIME_DIR"])
SOCKET = RUNTIME / "agentos.sock"
CONTROL = RUNTIME / "control.sock"

def send(path, data):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(8)
        sock.connect(str(path))
        sock.sendall(json.dumps(data, ensure_ascii=True).encode() + b"\n")
        out=b""
        while not out.endswith(b"\n"):
            part=sock.recv(65536)
            if not part: raise AssertionError("closed control socket")
            out+=part
        return json.loads(out)

def control(op, **fields):
    return send(CONTROL,{"version":1,"operation":op,**fields})

def infer(agent, prompt="oxygen", operation="infer"):
    return send(SOCKET,{"version":1,"request_id":"p1-"+agent,
         "agent_id":agent,"operation":operation,"payload":{"prompt":prompt}})

def ready():
    for _ in range(120):
        if SOCKET.exists() and CONTROL.exists(): return
        time.sleep(.03)
    raise AssertionError("server did not start")

p = subprocess.Popen([str(ROOT / "build/agentos")],cwd=ROOT,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
try:
    ready()
    data=control("list");assert data["status"]=="ok" and not data["processes"]
    assert infer("medical-agent")["status"]=="ok"
    a=control("list")["processes"]
    assert len(a)==1 and a[0]["pid"]==1 and a[0]["state"]=="WAITING" and a[0]["wait_reason"]=="AWAITING_WORK",a
    assert infer("medical-agent", "reactor")["status"]=="ok"
    assert len(control("list")["processes"])==1
    ev=control("events")["events"]
    assert any(x["event"]=="AGENT_CREATED" for x in ev)
    assert any(x["process"]["wait_reason"]=="INFERENCE" for x in ev if "process" in x)
    assert control("add", agent_id="research-agent",role="RESEARCH")["status"]=="ok"
    assert control("remove",agent_id="medical-agent")["status"]=="ok"
    res=infer("medical-agent");assert res["error"]["code"]=="AGENT_DISABLED",res
    zombie=next(x for x in control("list")["processes"] if x["agent_id"]=="medical-agent")
    assert zombie["state"]=="TERMINATED"
    assert control("reactivate",agent_id="medical-agent")["code"]=="DUPLICATE"
    assert control("reap",pid=zombie["pid"])["status"]=="ok"
    assert "medical-agent" in control("list")["disabled"]
    res=infer("medical-agent");assert res["error"]["code"]=="AGENT_DISABLED"
    fresh=control("reactivate",agent_id="medical-agent")
    assert fresh["status"]=="ok" and fresh["process"]["pid"]>zombie["pid"]
    assert infer("medical-agent")["status"]=="ok"
    assert control("remove",agent_id="ghost-id")["status"]=="ok"
    assert infer("ghost-id")["error"]["code"]=="AGENT_DISABLED"
    assert control("list")["disabled"]==["ghost-id"]
    before=len(control("list")["processes"])
    malformed=send(SOCKET,{"version":1,"request_id":"x","agent_id":"junk-agent","operation":"danger","payload":{"prompt":"test"}})
    assert malformed["status"]=="error"
    assert len(control("list")["processes"])==before
    assert control("terminate",pid=fresh["process"]["pid"])["status"]=="ok"
    assert infer("medical-agent")["error"]["code"]=="AGENT_TERMINATED"
    assert control("wait",pid=fresh["process"]["pid"])["status"]=="ok"
    assert infer("medical-agent")["status"]=="ok" # ordinary terminate does not blacklist
    data=control("events",since=0);assert all(x["schema_version"]==1 for x in data["events"])
    assert data["events"]==sorted(data["events"], key=lambda x:x["seq"])
    assert control("add",agent_id="expired",expires_monotonic_ms=1)["code"]=="EXPIRED_NO_ACTION"
    assert not any(x["agent_id"]=="expired" for x in control("list")["processes"])

    # Live bridge / native process roster, with explicit same-origin requests.
    sys.path.insert(0,str(ROOT/"demo"))
    from mission_control import Handler
    server=ThreadingHTTPServer(("127.0.0.1",0),Handler)
    t=threading.Thread(target=server.serve_forever,daemon=True);t.start()
    try:
        base=f"http://127.0.0.1:{server.server_port}"
        for path in ("/processes","/station","/workbench","/"):
            with urlopen(base+path,timeout=3) as r:assert r.status==200
        with urlopen(base+"/api/processes",timeout=3) as r:
            out=json.load(r);assert out["status"]=="ok"
        body=json.dumps({"operation":"add","agent_id":"ui-agent"}).encode()
        req=Request(base+"/api/process-action",body,method="POST",
              headers={"Content-Type":"application/json","Origin":base,"X-AgentOS-Control":"1"})
        with urlopen(req,timeout=3) as r:assert json.load(r)["status"]=="ok"
        req=Request(base+"/api/process-action",body,method="POST",
              headers={"Content-Type":"application/json","Origin":"http://evil.invalid","X-AgentOS-Control":"1"})
        try:urlopen(req,timeout=3);raise AssertionError("cross-origin control accepted")
        except HTTPError as e:assert e.code==403
        req=Request(base+"/api/process-action",body,method="POST",
              headers={"Content-Type":"application/json","Origin":base})
        try:urlopen(req,timeout=3);raise AssertionError("missing control header accepted")
        except HTTPError as e:assert e.code==403
        out=control("list");assert any(x["agent_id"]=="ui-agent" for x in out["processes"])
    finally:
        server.shutdown();server.server_close();t.join(timeout=2)
    print("PASS: process registration, single-instance reuse, native events, zombies, removal, fresh PIDs, control security and station monitor")
finally:
    p.send_signal(signal.SIGTERM)
    try:p.communicate(timeout=5)
    except subprocess.TimeoutExpired:p.kill();p.communicate()
    if p.returncode not in (0,-signal.SIGTERM):
        print("Server stderr",file=sys.stderr)
        # print only on failure
        raise AssertionError(p.returncode)

# New C program instance must have an empty in-memory registry and tombstone set.
new=subprocess.Popen([str(ROOT / "build/agentos")],cwd=ROOT,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
try:
    ready()
    fresh=control("list")
    assert fresh["status"]=="ok" and fresh["processes"]==[] and fresh["disabled"]==[],fresh
    assert infer("ghost-id")["status"]=="ok"
    assert control("list")["processes"][0]["pid"]==1
    print("PASS: P1 restart clears transient PCB/disabled registry and restarts logical PID space")
finally:
    new.send_signal(signal.SIGTERM)
    try:new.communicate(timeout=5)
    except subprocess.TimeoutExpired:new.kill();new.communicate()

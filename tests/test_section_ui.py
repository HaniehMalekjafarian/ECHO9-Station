#!/usr/bin/env python3
"""Section-scoped browser task dispatch regression, with the actual C P1 ProcessManager.

P1's raw Unix protocol deliberately retains automatic agent registration.
This suite verifies that the *managed browser UI* requires an existing PCB,
proper section/role matching and a non-zombie state.
"""
import json
import os
import signal
import subprocess
import sys
import threading
import time
from http.server import ThreadingHTTPServer
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

ROOT=Path(__file__).resolve().parents[1]
RUNTIME=Path(os.environ['AGENTOS_RUNTIME_DIR'])
SOCKET=RUNTIME/'agentos.sock'
CONTROL=RUNTIME/'control.sock'


def wait_ready():
    for _ in range(130):
        if SOCKET.is_socket() and CONTROL.is_socket():
            return
        time.sleep(.03)
    raise AssertionError('C server did not create control and inference sockets')


def call(base,path,payload):
    data=json.dumps(payload).encode()
    req=Request(base+path,data=data,headers={'Content-Type':'application/json',
        'X-AgentOS-Control':'1','Origin':base},method='POST')
    try:
        with urlopen(req,timeout=8) as resp:
            return resp.status,json.load(resp)
    except HTTPError as err:
        return err.code,json.loads(err.read())


p=subprocess.Popen([str(ROOT/'build/agentos')],cwd=ROOT,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
try:
    wait_ready()
    sys.path.insert(0,str(ROOT/'demo'))
    from mission_control import Handler
    server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
    thread=threading.Thread(target=server.serve_forever,daemon=True)
    thread.start()
    base=f'http://127.0.0.1:{server.server_port}'
    try:
        for path in ('/station','/processes'):
            with urlopen(base+path,timeout=3) as r:
                page=r.read().decode()
            assert 'taskSection' in page or 'infer-section' in page
            assert 'ROLE_MISMATCH' not in page  # Backend enforces role, not only JavaScript.
            if path=='/station':
                assert all(tok in page for tok in ('SPAWN_MS','DUST_MS','TERMINATED','dustBurst','drawAgent'))
        task={'agent_id':'med-1','pid':1,'section':'MEDICAL','prompt':'Check crew oxygen'}
        code,body=call(base,'/api/agent-infer',task)
        assert code==409 and body['code']=='AGENT_NOT_REGISTERED',(code,body)
        with urlopen(base+'/api/processes',timeout=3) as r: roster=json.load(r)
        assert roster['processes']==[],roster
        code,body=call(base,'/api/process-action',{'operation':'add','agent_id':'med-1','role':'MEDICAL'})
        assert code==200 and body['status']=='ok',(code,body)
        pid=body['process']['pid'];task['pid']=pid
        code,body=call(base,'/api/agent-infer',{**task,'section':'ENGINEERING'})
        assert code==409 and body['code']=='ROLE_MISMATCH',(code,body)
        code,body=call(base,'/api/agent-infer',task)
        assert code==200 and body['response']['status']=='ok',(code,body)
        code,body=call(base,'/api/process-action',{'operation':'terminate','pid':pid})
        assert code==200,(code,body)
        code,body=call(base,'/api/agent-infer',task)
        assert code==409 and body['code']=='AGENT_TERMINATED',(code,body)
        code,body=call(base,'/api/process-action',{'operation':'reap','pid':pid})
        assert code==200,(code,body)
        code,body=call(base,'/api/agent-infer',task)
        assert code==409 and body['code']=='AGENT_NOT_REGISTERED',(code,body)
        code,body=call(base,'/api/process-action',{'operation':'add','agent_id':'tech-1','role':'MAINTENANCE'})
        assert code==200,(code,body)
        task2={'agent_id':'tech-1','pid':body['process']['pid'],'section':'ENGINEERING','prompt':'Check the reactor'}
        code,body=call(base,'/api/agent-infer',task2)
        assert code==200 and body['response']['status']=='ok',(code,body)
        code,body=call(base,'/api/agent-infer',{**task2,'pid':99999})
        assert code==409 and body['code']=='AGENT_NOT_REGISTERED',(code,body)
        print('PASS: station/process UI rejects unknown IDs, wrong PIDs, role mismatches and zombies')
        print('PASS: known Medical agent and MAINTENANCE -> Engineering alias accept task')
        print('PASS: station lifecycle visuals and role-aware controls included')
    finally:
        server.shutdown();server.server_close();thread.join(timeout=3)
finally:
    p.send_signal(signal.SIGTERM)
    try:p.communicate(timeout=5)
    except subprocess.TimeoutExpired:p.kill();p.communicate()

"""Smoke test for page serving and honest bridge-backed event lifecycle."""
import json
import os
import socket
import subprocess
import tempfile
import threading
import time
import urllib.request
from pathlib import Path

BASE = Path(__file__).resolve().parents[2]


def run():
    with tempfile.TemporaryDirectory() as temp:
        socket_path = temp + '/agentos.sock'
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(socket_path)
        listener.listen()
        requests = []
        def fake_agentos():
            while True:
                try:
                    conn, _ = listener.accept()
                except OSError:
                    return
                with conn:
                    buf=b''
                    while not buf.endswith(b'\n'):
                        data=conn.recv(4096)
                        if not data: break
                        buf+=data
                    if not buf: continue
                    req=json.loads(buf)
                    requests.append(req)
                    time.sleep(.35)
                    out={'version':1,'request_id':req['request_id'],'status':'ok','result':{'text':'Fake inference for UI smoke test'}}
                    conn.sendall((json.dumps(out)+'\n').encode())
        threading.Thread(target=fake_agentos,daemon=True).start()
        # Choose a free local port instead of assuming 18765 is unused.
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.bind(('127.0.0.1', 0))
            port=probe.getsockname()[1]
        env={**os.environ,'AGENTOS_SOCKET':socket_path,'AGENTOS_UI_PORT':str(port),'AGENTOS_UI_BIND':'127.0.0.1'}
        server=subprocess.Popen(['python3',str(BASE/'demo/mission_control.py')],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        try:
            url=f'http://127.0.0.1:{port}'
            last_error = None
            deadline=time.monotonic()+12
            while time.monotonic()<deadline:
                if server.poll() is not None:
                    raise RuntimeError('UI server exited before startup')
                try:
                    page=urllib.request.urlopen(url+'/station',timeout=.5).read().decode()
                    break
                except OSError as exc:
                    last_error = exc
                    time.sleep(.05)
            else: raise RuntimeError(f'UI server failed to start within 12 seconds: {last_error!r}; poll={server.poll()}')
            assert 'Live Station View' in page
            assert 'canvas id="station"' in page
            old=urllib.request.urlopen(url+'/').read().decode()
            assert 'OPEN ECHO-9 STATION VIEW' in old
            status=json.load(urllib.request.urlopen(url+'/api/status'))
            assert status['socket_present'] is True
            data=json.dumps({'agent':'research','prompt':'Check station conditions'}).encode()
            with urllib.request.urlopen(urllib.request.Request(url+'/api/request',data=data,headers={'Content-Type':'application/json'}),timeout=6) as res:
                reply=json.load(res)
            assert reply['response']['result']['text']=='Fake inference for UI smoke test'
            status=json.load(urllib.request.urlopen(url+'/api/status'))
            # Real raw-workbench requests also reach the observed Unix socket.
            wb=json.dumps({'frame':json.dumps({'version':1,'request_id':'wb-integration','agent_id':'research-agent','operation':'infer','payload':{'prompt':'Inspect'}}),'append_newline':True,'fragment_size':5,'halfclose':False}).encode()
            with urllib.request.urlopen(urllib.request.Request(url+'/api/workbench',data=wb,headers={'Content-Type':'application/json'}),timeout=6) as res:
                probe=json.load(res)
            assert probe['response']['request_id']=='wb-integration',probe
            assert probe['transport_error'] is None,probe
            assert len(requests)==2
            assert status['events'][0]['kind']=='COMPLETED'
            assert status['events'][1]['kind']=='ARRIVED'
            assert status['events'][0]['agent']=='research'
            assert isinstance(status['events'][0]['at_ms'],int)
            print('PASS: original dashboard still works')
            print('PASS: station HTML served on /station')
            print('PASS: request forwarded to Unix socket and received')
            print('PASS: Workbench fragmented frame reaches the real Unix socket')
            print('PASS: two genuine bridge events with timestamp')
        finally:
            server.terminate()
            try: server.wait(timeout=2)
            except subprocess.TimeoutExpired: server.kill(); server.wait()
            listener.close()

if __name__=='__main__':run()

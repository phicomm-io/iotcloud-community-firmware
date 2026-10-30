#!/usr/bin/env python3
import argparse,os,subprocess,shutil
from pathlib import Path
from test_network import Broker,free_port
p=argparse.ArgumentParser();p.add_argument('host',type=Path);p.add_argument('run',type=Path);a=p.parse_args();r=a.run.resolve();b=Broker();port=free_port();env=dict(os.environ,PH_HOST_EMPTY_PASSWORD='1');log=(r/'browser-server.log').open('w');store=r/'browser-storage';shutil.rmtree(store,ignore_errors=True);store.mkdir()
s=subprocess.Popen([str(a.host.resolve()),str(b.port),str(port),str(store)],env=env,stdout=subprocess.PIPE,stderr=log,text=True)
try:
 assert s.stdout.readline().strip()=='READY'
 b.wait('/availability',lambda p:p==b'online')
 subprocess.run(['node',str(Path(__file__).with_name('web.cjs')),f'http://127.0.0.1:{port}',str(r)],check=True,timeout=180)
finally:
 s.terminate();s.wait(5);b.close();log.close()
 assert 'Sanitizer' not in (r/'browser-server.log').read_text()

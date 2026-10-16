#!/usr/bin/env python3
import json
import os
import signal
import time
import urllib.request
from scenarios import call,wait,record,URL

def main():
    nodes=[n for n in json.load(urllib.request.urlopen(URL)) if n['id'] in [f'02000001000{i}' for i in range(3,7)]]
    members=[{'mac':n['id'],'ip':n['ip']} for n in nodes]
    for n in nodes:
        call(n['id'],'linkage.set',{'enabled':0})
        call(n['id'],'binding.set',{'group':'stability-group','members':members,'group_recovery_seconds':5})
    leader=nodes[0];follower=nodes[1]
    # Only the coordinator proactively joins; followers establish LAN on their
    # first group command/ack. Do not require an unused follower path to be joined.
    wait(leader['id'],'binding.status.get',lambda s:len(s['members'])==4 and all(m['mac']==leader['id'] or m['transport']=='lan' for m in s['members']),seconds=90)
    call(leader['id'],'fan.set',{'speed':42})
    for n in nodes:record('group-fan-out',node=n['id'],status=wait(n['id'],'status.get',lambda s:s['speed']==42))
    call(leader['id'],'light_effect.set',{'effect':3})
    for n in nodes:record('group-config-fan-out',node=n['id'],config=wait(n['id'],'light_effect.get',lambda s:s['effect']==3))
    for cycle in range(20):
        try:
            os.kill(leader['pid'],signal.SIGSTOP)
            state=wait(follower['id'],'status.get',lambda s:s['coordinator'],seconds=25)
            call(follower['id'],'fan.set',{'speed':43+cycle%2})
            for n in nodes[1:]:wait(n['id'],'status.get',lambda s:s['speed']==43+cycle%2,seconds=20)
            record('coordinator-offline-takeover',cycle=cycle,status=state)
        finally:os.kill(leader['pid'],signal.SIGCONT)
        wait(follower['id'],'status.get',lambda s:not s['coordinator'],seconds=25)
        wait(leader['id'],'status.get',lambda s:s['coordinator'],seconds=25)
        record('coordinator-rejoined',cycle=cycle)
        print('coordinator cycle',cycle,flush=True)
    record('group-scenarios-pass')
if __name__=='__main__':
    try:main()
    except Exception as e:record('group-failure',error=str(e));raise

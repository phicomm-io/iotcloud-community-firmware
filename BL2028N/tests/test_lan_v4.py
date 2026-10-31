#!/usr/bin/env python3
"""Independent S30 shared-v4 socket qualification, no DUT or firmware writes."""
import argparse
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import unittest
import msgpack

ROOT=Path(__file__).resolve().parents[2]
HERE=Path(__file__).resolve().parent
RUN=None

def pack(value):return msgpack.packb(value,use_bin_type=True)
def unpack(value):return msgpack.unpackb(value,raw=False)

def build(run):
    run.mkdir(parents=True,exist_ok=True)
    command=[shutil.which('clang') or 'cc','-std=gnu99','-g','-O1','-Wall','-Wextra','-Werror',
        '-Wno-misleading-indentation','-Wno-sign-compare','-fsanitize=address,undefined','-fno-omit-frame-pointer',
        '-DPH_S30_LAN_V4',str(HERE/'lan_v4_host.c'),str(ROOT/'BL2028N/protocol/local_auth.c'),
        *[str(ROOT/'BL2028N/src'/name) for name in ('ph_lan_v4.c','ph_wire.c','ph_device.c','ph_meter.c')],
        '-lcrypto','-lm','-o',str(run/'lan-v4-host')]
    (run/'compiler-command.txt').write_text('\n'.join(command)+'\n')
    with (run/'compiler.log').open('w') as log:subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)

def build_host(run):
    build(run)
    return run/'lan-v4-host'

class LanV4(unittest.TestCase):
    def setUp(self):
        self.log=(RUN/(self._testMethodName+'.stderr')).open('w')
        self.process=subprocess.Popen([str(RUN/'lan-v4-host')],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=self.log,text=True,bufsize=1)
        line=self.process.stdout.readline().strip();match=re.fullmatch(r'PORT (\d+) ID ([0-9a-f]{12}) PASSWORD (.+)',line)
        self.assertIsNotNone(match,line);self.address=('127.0.0.1',int(match[1]));self.local=match[2];self.password=match[3]
        self.sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);self.sock.bind(('127.0.0.1',0));self.sock.settimeout(.3)
        self.peer='02ffeedd0001';self.boot=77;self.counter=0
    def tearDown(self):
        self.sock.close()
        if self.process.poll() is None:self.process.stdin.write('STOP\n');self.process.stdin.flush()
        self.assertEqual(self.process.wait(timeout=5),0);self.process.stdin.close();self.process.stdout.close();self.log.close()
    def console(self,line):
        self.process.stdin.write(line+'\n');self.process.stdin.flush();return self.process.stdout.readline().strip()
    def counters(self):return {k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',self.console('COUNTERS'))}
    def remote(self,rid,action,params):
        line=self.console('REMOTE '+pack(dict(request_id=rid,action=action,params=params)).hex())
        self.assertTrue(line.startswith('REMOTE '),line);return unpack(bytes.fromhex(line[7:]))
    def exchange(self,value,expect=True):
        self.sock.sendto(value if isinstance(value,bytes) else pack(value),self.address)
        if not expect:
            with self.assertRaises(socket.timeout):self.sock.recvfrom(1200)
            return None
        reply,source=self.sock.recvfrom(1200);self.assertEqual(source,self.address);return unpack(reply)
    def pair(self,peer=None,scopes=1):
        peer=peer or self.peer;nonce=os.urandom(16)
        r=self.exchange(dict(op='auth_pair',src=peer,password=self.password,scopes=scopes,nonce=nonce))
        self.assertEqual((r['code'],r['nonce'],r['src']),('ok',nonce,self.local));self.assertIsInstance(r['key'],bytes);return r['key']
    def resume(self,key,peer=None,boot=None):
        peer=peer or self.peer;nonce=os.urandom(16)
        r=self.exchange(dict(op='auth_resume',src=peer,key=key,nonce=nonce,boot=self.boot if boot is None else boot))
        self.assertEqual((r['code'],r['nonce']),('ok',nonce));return r['token']
    def command(self,token,id=1,action='status.get',params=None,rid=None,peer=None,boot=None):
        return dict(v=4,src=peer or self.peer,dst=self.local,boot=self.boot if boot is None else boot,id=id,
            op='command',token=token,body=pack(dict(request_id=rid or 'lan-'+str(id),action=action,params=params or {})))
    def control(self,request,op,**extra):
        result={k:v for k,v in request.items() if k not in ('body','op')};result.update(op=op,**extra);return result
    def result(self,request,ack=True):
        first=self.exchange(request);self.assertEqual(first['code'],'ok',first)
        pieces=[first['body']]
        for part in range(1,first['total']):
            reply=self.exchange(self.control(request,'pull',part=part));self.assertEqual(reply['part'],part);pieces.append(reply['body'])
        body=b''.join(pieces);self.assertEqual(len(body),first['size'])
        if ack:self.assertEqual(self.exchange(self.control(request,'ack'))['code'],'acked')
        return unpack(body),first
    def test_pair_ephemeral_port_status_and_reassembly(self):
        token=self.resume(self.pair());request=self.command(token)
        reply,frame=self.result(request)
        self.assertEqual(reply['code'],'ok');self.assertEqual(reply['data']['socket_states'],[False]);self.assertEqual(reply['request_id'],'lan-1')
        self.assertEqual((frame['src'],frame['dst'],frame['request_boot']),(self.local,self.peer,self.boot))
        self.assertNotEqual(self.sock.getsockname()[1],11288)
    def test_duplicate_conflict_ack_and_burned_highwater(self):
        token=self.resume(self.pair());request=self.command(token,action='outlet.set',params={'index':0,'on':True})
        a=self.exchange(request);b=self.exchange(request);self.assertEqual(a,b);self.assertEqual(self.counters()['relay'],1)
        changed=dict(request,body=pack(dict(request_id='lan-1',action='outlet.set',params={'index':0,'on':False})))
        self.assertEqual(self.exchange(changed)['code'],'conflict');self.assertEqual(self.counters()['relay'],1)
        self.assertEqual(self.exchange(self.control(request,'ack'))['code'],'acked')
        self.assertEqual(self.exchange(self.control(request,'ack'))['code'],'acked')
        self.assertEqual(self.exchange(request)['code'],'expired');self.assertEqual(self.counters()['relay'],1)
    def test_shared_result_owner_busy_and_response_independent(self):
        first=self.command(self.resume(self.pair()));a=self.exchange(first)
        peer='02ffeedd0002';second=self.command(self.resume(self.pair(peer),peer),peer=peer)
        self.assertEqual(self.exchange(second)['code'],'busy');self.assertEqual(self.exchange(first),a)
        self.assertEqual(self.exchange(self.control(first,'pull',part=10))['code'],'invalid_params')
        self.exchange(self.control(first,'ack'));self.result(second)
    def test_resume_rotation_sensor_scope_and_unpair_retry(self):
        key=self.pair();old=self.resume(key);new=self.resume(key,boot=78);self.assertNotEqual(old,new)
        self.assertEqual(self.exchange(self.command(old))['code'],'auth_required')
        request=self.command(new,boot=78);self.result(request)
        nonce=os.urandom(16);remove=dict(op='auth_unpair',src=self.peer,token=new,nonce=nonce)
        self.assertEqual(self.exchange(remove)['code'],'ok');self.assertEqual(self.exchange(remove)['code'],'ok')
        self.assertEqual(self.exchange(dict(remove,nonce=os.urandom(16)))['code'],'auth_required')
        self.assertEqual(self.exchange(self.command(new,id=2,boot=78))['code'],'auth_required')
        token=self.resume(self.pair(scopes=2));reply,_=self.result(self.command(token))
        self.assertEqual(reply['code'],'ok')
        denied,_=self.result(self.command(token,id=2,action='outlet.set',params={'on':True}))
        self.assertEqual(denied['code'],'unauthorized');self.assertEqual(self.counters()['relay'],0)
    def test_low_heap_before_side_effect_and_post_dispatch_failure(self):
        token=self.resume(self.pair());request=self.command(token,action='outlet.set',params={'on':True})
        self.console('HEAP 7680');self.assertEqual(self.exchange(request)['code'],'busy');self.assertEqual(self.counters()['relay'],0)
        self.console('HEAP 32768');self.console('FAIL_REPLY');self.assertEqual(self.exchange(request)['code'],'expired')
        self.assertEqual(self.counters()['relay'],1);self.assertEqual(self.exchange(request)['code'],'expired');self.assertEqual(self.counters()['relay'],1)
    def test_pause_expiry_and_revoke_release(self):
        token=self.resume(self.pair());request=self.command(token);self.exchange(request);self.assertGreater(self.counters()['retained'],0)
        self.console('PAUSE');self.assertEqual(self.counters()['retained'],0);self.assertEqual(self.exchange(request)['code'],'expired')
        request=self.command(token,id=2);self.exchange(request);self.console('ADVANCE 5001');self.assertEqual(self.counters()['retained'],0)
        self.assertEqual(self.exchange(request)['code'],'expired');self.console('REVOKE')
        self.assertEqual(self.exchange(self.command(token,id=3))['code'],'auth_required')
    def test_allowlist_and_old_or_malformed_envelopes_rejected(self):
        token=self.resume(self.pair());request=self.command(token,action='system.reset');r,_=self.result(request);self.assertEqual(r['code'],'unsupported')
        self.assertEqual(self.counters()['dispatch'],0)
        self.exchange(dict(request,v=2),expect=False);self.exchange(dict(request,token=token.hex()),expect=False)
        self.exchange(b'{"v":4}',expect=False);self.exchange(pack(dict(request,extra=1)),expect=False)
        self.exchange(pack(dict(op='auth_pair',src=self.peer,password=self.password,scopes=1,nonce='x'*16)),expect=False)
    def test_aliased_pair_and_business_input_workspace(self):
        self.console('ALIAS');key=self.pair();self.console('ALIAS');token=self.resume(key)
        self.console('ALIAS');reply,_=self.result(self.command(token));self.assertEqual(reply['data']['socket_states'],[False])
    def test_four_sessions_and_revoked_slot_reclamation(self):
        peers=['02ffeedd000'+str(i) for i in range(1,6)];tokens=[]
        for peer in peers:
            tokens.append(self.resume(self.pair(peer),peer))
        for peer,token in zip(peers[:4],tokens[:4]):self.result(self.command(token,peer=peer))
        fifth=self.command(tokens[4],peer=peers[4]);self.assertEqual(self.exchange(fifth)['code'],'busy')
        self.assertEqual(self.counters()['dispatch'],4)
        self.assertEqual(self.exchange(dict(op='auth_unpair',src=peers[0],token=tokens[0],nonce=os.urandom(16)))['code'],'ok')
        self.result(fifth);self.assertEqual(self.counters()['dispatch'],5)
    def test_receipts_are_scoped_to_peer_and_remote_domain(self):
        token=self.resume(self.pair());rid='same-wire-id'
        reply,_=self.result(self.command(token,action='outlet.set',params={'on':True},rid=rid))
        self.assertEqual((reply['code'],reply['request_id']),('accepted',rid));self.assertEqual(self.counters()['relay'],1)
        internal_key=self.console('KEY').split()[1];self.assertRegex(internal_key,r'^[0-9a-f]{64}$')
        # Even a raw HTTP/MQTT request ID equal to the LAN hash is another domain.
        self.assertEqual(self.remote(internal_key,'outlet.set',{'on':False})['code'],'accepted')
        self.assertEqual(self.counters()['relay'],2)
        replay,_=self.result(self.command(token,id=2,action='outlet.set',params={'on':True},rid=rid))
        self.assertEqual(replay['data']['on'],True);self.assertEqual(self.counters()['relay'],2)
        self.assertEqual(self.remote(internal_key,'outlet.set',{'on':False})['code'],'accepted')
        self.assertEqual(self.counters()['relay'],2)
        peer='02ffeedd0002';other=self.resume(self.pair(peer),peer)
        reply,_=self.result(self.command(other,action='outlet.set',params={'on':False},rid=rid,peer=peer))
        self.assertEqual(reply['code'],'accepted');self.assertEqual(self.counters()['relay'],3)
        conflict,_=self.result(self.command(token,id=3,action='outlet.set',params={'on':False},rid=rid))
        self.assertEqual(conflict['code'],'conflict');self.assertEqual(self.counters()['relay'],3)
    def test_control_scope_indicator_child_lock_and_admin_policy(self):
        token=self.resume(self.pair())
        for id,action in ((1,'indicator.set'),(2,'child_lock.set')):
            reply,_=self.result(self.command(token,id=id,action=action,params={'enabled':False if id==1 else True}))
            self.assertEqual(reply['code'],'accepted')
        status,_=self.result(self.command(token,id=3))
        self.assertEqual((status['data']['indicator'],status['data']['child_lock']),(False,True))
        self.assertEqual(status['data']['revision'],2)
        before=self.counters()['dispatch']
        for id,action in enumerate(('config.set','system.reset','system.reboot','ota.start'),4):
            reply,_=self.result(self.command(token,id=id,action=action))
            self.assertEqual(reply['code'],'unsupported')
        self.assertEqual(self.counters()['dispatch'],before)
        unsupported,_=self.result(self.command(token,id=8,action='fan.set',params={'on':True}))
        self.assertEqual(unsupported['code'],'unsupported')
    def test_broker_granted_sensor_token_cannot_control(self):
        token=bytes.fromhex('a5'*16)
        grant='SENSOR_GRANT '+self.peer+' '+token.hex()+' '
        self.assertEqual(self.console(grant+'0'),'SENSOR_GRANT DENIED')
        self.assertEqual(self.console(grant+str(self.boot)),'SENSOR_GRANT OK')
        # A valid token with a wrong first boot must not occupy the peer's
        # replay session. Its authoritative boot can still start at ID 1.
        self.assertEqual(self.exchange(self.command(token,boot=self.boot+1))['code'],'auth_required')
        self.assertEqual(self.exchange(self.command(bytes.fromhex('a6'*16)))['code'],'auth_required')
        for id,action in enumerate(('status.get','info.get','capabilities.get','diagnostics.get','power.get'),1):
            reply,_=self.result(self.command(token,id=id,action=action));self.assertEqual(reply['code'],'ok')
        for id,action,params in ((6,'outlet.set',{'on':True}),(7,'indicator.set',{'enabled':False}),
                                 (8,'child_lock.set',{'enabled':True}),(9,'config.set',{}),(10,'name.set',{'name':'Forbidden'})):
            reply,_=self.result(self.command(token,id=id,action=action,params=params))
            self.assertEqual(reply['code'],'unsupported' if action=='config.set' else 'unauthorized')
        status,_=self.result(self.command(token,id=11))
        self.assertEqual((status['data']['socket_states'],status['data']['indicator'],status['data']['child_lock']),([False],True,False))
        self.assertEqual(self.counters()['relay'],0)
        # MQTT closure revokes only MQTT credentials/results. Local pairing
        # and its high-water mark remain, including an ACKed mutation ID.
        peer='02ffeedd0002';local=self.resume(self.pair(peer),peer)
        prior=self.command(local,peer=peer,action='outlet.set',params={'on':True})
        self.result(prior);self.assertEqual(self.counters()['relay'],1)
        self.assertEqual(self.console('MQTT_CLOSE'),'MQTT_CLOSE OK')
        self.assertEqual(self.exchange(self.command(token,id=12))['code'],'auth_required')
        self.assertEqual(self.exchange(prior)['code'],'expired');self.assertEqual(self.counters()['relay'],1)
        self.result(self.command(local,id=2,peer=peer))
        # Explicit CONTROL injection is a core fixture only; the product's
        # sensor grant API above cannot grant this scope.
        token=bytes.fromhex('5a'*16)
        self.assertEqual(self.console('GRANT '+self.peer+' '+token.hex()+' 1'),'GRANT OK')
        reply,_=self.result(self.command(token,action='outlet.set',params={'on':False}))
        self.assertEqual(reply['code'],'accepted');self.assertEqual(self.counters()['relay'],2)
    def test_control_name_write_readback_replay_and_scope(self):
        token=self.resume(self.pair())
        name='客厅, Desk: [1] \\灯'
        request=self.command(token,action='name.set',params={'name':name},rid='rename')
        reply,_=self.result(request)
        self.assertEqual((reply['code'],reply['data']['name']),('accepted',name))
        status,_=self.result(self.command(token,id=2))
        self.assertEqual(status['data']['name'],name)
        revision=status['data']['revision']
        repeated,_=self.result(self.command(token,id=3,action='name.set',params={'name':name},rid='rename'))
        self.assertEqual(repeated,reply)
        conflict,_=self.result(self.command(token,id=4,action='name.set',params={'name':'Other'},rid='rename'))
        self.assertEqual(conflict['code'],'conflict')
        invalid,_=self.result(self.command(token,id=5,action='name.set',params={'name':'x'*64}))
        self.assertEqual(invalid['code'],'invalid_params')
        forbidden,_=self.result(self.command(token,id=6,action='config.set',params={'section':'device','values':{'name':'Other'}}))
        self.assertEqual(forbidden['code'],'unsupported')
        status,_=self.result(self.command(token,id=7))
        self.assertEqual((status['data']['name'],status['data']['revision']),(name,revision))
        self.assertEqual(self.counters()['relay'],0)

def main():
    global RUN
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--run-dir',type=Path,required=True);parser.add_argument('--build-only',action='store_true');args=parser.parse_args()
    RUN=args.run_dir.resolve();build(RUN)
    if args.build_only:print(RUN/'lan-v4-host');return
    unittest.main(argv=[__file__],verbosity=2)

if __name__=='__main__':main()

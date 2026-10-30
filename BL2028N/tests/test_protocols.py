#!/usr/bin/env python3
"""Independent Python MessagePack/CBOR clients drive sanitized product C code."""
import argparse
import math
import os
from pathlib import Path
import random
import subprocess
import unittest
import msgpack
import cbor2

PACK={1:lambda x:msgpack.packb(x,use_bin_type=True),2:cbor2.dumps}
UNPACK={1:lambda x:msgpack.unpackb(x,raw=False),2:cbor2.loads}
HOST=None

class Core(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.p=subprocess.Popen([HOST],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
    @classmethod
    def tearDownClass(cls):
        cls.p.stdin.close()
        if cls.p.wait(timeout=10):raise AssertionError('Sanitized C process failed')
    def call(self,line):
        self.p.stdin.write(line+'\n');self.p.stdin.flush()
        response=self.p.stdout.readline()
        self.assertTrue(response,'C process exited unexpectedly')
        return bytes.fromhex(response.strip())
    def mp(self,line):return UNPACK[1](self.call(line))
    def setUp(self):self.mp('R')
    def command(self,action,params,rid='test-1',i=1,o=1,auth=True,admin=True):
        raw=PACK[i]({'request_id':rid,'action':action,'params':params})
        result=self.call(f'C {i} {o} {int(auth)} {int(admin)} {raw.hex()}')
        return UNPACK[o](result)
    def query(self,action='status.get',f=1):return UNPACK[f](self.call(f'Q {f} {action}'))
    def packet(self,op,body=None,token='',id=1,boot=777,dst='aabbccddeeff',f=1,**extra):
        p={'v':2,'src':'001122334455','dst':dst,'boot':boot,'id':id,'op':op,'token':token,'body':body or {}}
        p.update(extra);r=self.call('U '+PACK[f](p).hex())
        return UNPACK[f](r) if r else None
    def paired(self,f=1):
        p=self.packet('auth.pair',{'peer':'001122334455','password':'secret','scopes':7},f=f)
        key=p['body']['key'];r=self.packet('auth.resume',{'peer':'001122334455','key':key},id=2,f=f)
        return key,r['body']['token']
    def test_shared_frontend_contract(self):
        raw=PACK[1]({'action':'status.get','params':{'view':'live'}})
        reply=UNPACK[1](self.call('C 1 1 1 1 '+raw.hex()))
        self.assertEqual(reply['code'],'ok')
        self.assertEqual(reply['data']['socket_states'],[False])
        self.assertEqual(reply['data']['report_seconds'],30)
        raw=PACK[1]({'action':'outlet.set','params':{'on':True}})
        self.assertEqual(UNPACK[1](self.call('C 1 1 1 1 '+raw.hex()))['code'],'invalid_params')
        self.assertFalse(self.query()['power'])
        self.assertEqual(self.command('config.set',{'revision':0,'power_on':True})['code'],'invalid_params')
    def test_binary_interoperability(self):
        for i in (1,2):
            for o in (1,2):
                reply=self.command('outlet.set',{'index':0,'on':True},rid=f'{i}{o}',i=i,o=o)
                self.assertEqual(reply['code'],'accepted');self.assertTrue(reply['data']['on'])
                status=self.query(f=o);self.assertTrue(status['power']);self.assertEqual(status['mac'],'aabbccddeeff')
                self.assertEqual(status['metering']['voltage'],None)
    def test_idempotency_and_conflict(self):
        first=self.command('config.set',{'section':'device','values':{'name':'Desk'}},i=1)
        before=self.mp('S');second=self.command('config.set',{'values':{'name':'Desk'},'section':'device'},i=2,o=2)
        self.assertEqual(first,second);self.assertEqual(before,self.mp('S'))
        self.assertEqual(self.command('config.set',{'section':'device','values':{'name':'Other'}})['code'],'conflict')
        self.assertEqual(self.command('config.set',{'section':'device','values':{'name':'Other'}},rid='new')['code'],'accepted')
        self.assertEqual(self.query('config.get')['name'],'Other')
    def test_failure_is_atomic(self):
        before=self.query('config.get');self.mp('F 1')
        self.assertEqual(self.command('config.set',{'section':'device','values':{'power_on':True}})['code'],'storage_error')
        self.assertEqual(self.query('config.get'),before)
        self.mp('F 0');self.assertEqual(self.command('config.set',{'section':'device','values':{'power_on':True}},rid='retry')['code'],'accepted')
        self.mp('X');self.assertTrue(self.query()['power'])
    def test_authz_and_local_priority(self):
        self.assertEqual(self.command('outlet.set',{'on':True},auth=False)['code'],'unauthorized')
        self.assertEqual(self.command('system.reboot',{},admin=False)['code'],'unauthorized')
        self.mp('L');self.assertEqual(self.command('outlet.set',{'on':False})['code'],'busy')
        self.mp('T 11500');self.assertEqual(self.command('outlet.set',{'on':False})['code'],'accepted')
        self.assertFalse(self.query()['power'])
        self.assertEqual(self.command('child_lock.set',{'enabled':True},rid='lock')['code'],'accepted')
        self.mp('L');self.assertFalse(self.query()['power'])
    def test_strict_inputs(self):
        for params in ({'on':1},{'on':'true'},{'on':True,'index':1},{'on':True,'unknown':0}):
            self.assertEqual(self.command('outlet.set',params,rid='bad-'+str(len(str(params))))['code'],'invalid_params')
        self.assertFalse(self.query()['power'])
        for f,raw in [(0,b'{}'),(1,b'\x82\xa1a\xc3\xa1a\xc2'),(2,b'\xa2\x61a\xf5\x61a\xf4'),(1,b'\x81\xa1a\xcb'+bytes.fromhex('7ff0000000000000')),(2,b'\xbf\xff'),(1,b'\x81\xa1a\xc4\x01x'),(1,b'\x81\xa1a\xa1\xff')]:
            self.assertEqual(self.call(f'V {f} {raw.hex()}'),b'\0')
        for f in (1,2):
            for obj in ({'k':None},{'k':True},{'k':-9007199254740991},{'k':9007199254740991},{'文本':'电量'}):
                self.assertEqual(self.call(f'V {f} {PACK[f](obj).hex()}'),b'\1')
        for f in (1,2):
            self.assertEqual(self.call(f'V {f} {PACK[f]({"k":9007199254740992}).hex()}'),b'\0')
        self.assertEqual(self.call('Q 0 status.get'),b'')
    def test_http_auth_library(self):
        self.assertEqual(self.call('H wrong'),b'')
        token=self.call('H secret').decode();self.assertEqual(len(token),32)
        self.assertEqual(self.call('B '+token),b'\1')
        self.assertEqual(self.call('O '+token),b'\1');self.assertEqual(self.call('B '+token),b'\0')
        token=self.call('H secret').decode();self.mp('T 1810000');self.assertEqual(self.call('B '+token),b'\0')
    def test_http_sessions_replace_stale_tabs(self):
        tokens=[]
        for i in range(4):
            self.mp('T '+str(10000+i*100));tokens.append(self.call('H secret').decode())
        self.mp('T 10500');self.assertEqual(self.call('B '+tokens[0]),b'\1')
        self.assertEqual(self.call('H wrong'),b'')
        self.assertTrue(all(self.call('B '+t)==b'\1' for t in tokens))
        # Make the second session least recently used after the validity checks.
        self.mp('T 10600')
        for t in (tokens[0],tokens[2],tokens[3]):self.assertEqual(self.call('B '+t),b'\1')
        fresh=self.call('H secret').decode();self.assertEqual(len(fresh),32)
        self.assertEqual(self.call('B '+tokens[1]),b'\0')
        for t in (tokens[0],tokens[2],tokens[3],fresh):self.assertEqual(self.call('B '+t),b'\1')
    def test_udp_pair_resume_restart_revoke(self):
        for f in (1,2):
            key,token=self.paired(f)
            reply=self.packet('command',{'request_id':'udp-on','action':'outlet.set','params':{'on':True}},token,id=3,f=f,group='')
            self.assertEqual(reply['body']['code'],'accepted');self.assertEqual(reply['token'],token)
            self.mp('X');self.assertIsNone(self.packet('read',token=token,id=4,f=f))
            resumed=self.packet('auth.resume',{'peer':'001122334455','key':key},f=f)
            self.assertNotEqual(token,resumed['body']['token'])
            self.assertEqual(self.call('W changed'),b'\1');self.mp('X')
            self.assertEqual(self.packet('auth.resume',{'peer':'001122334455','key':key},f=f)['body']['error'],'unauthorized')
            self.call('W secret')
    def test_udp_replay_and_broadcast(self):
        key,token=self.paired()
        body={'request_id':'udp','action':'outlet.set','params':{'on':True}}
        self.assertIsNone(self.packet('command',body,token,dst='broadcast'))
        first=self.packet('command',body,token,id=100);self.assertEqual(first['body']['code'],'accepted')
        self.assertIsNone(self.packet('command',body,token,id=100));self.assertIsNone(self.packet('command',body,token,id=99))
        self.assertIsNone(self.packet('command',body,token,id=101,boot=778))
        self.assertEqual(self.packet('probe',token=token,id=101)['op'],'pong')
        self.assertIsNotNone(self.packet('discovery',dst='broadcast'))
        resumed=self.packet('auth.resume',{'peer':'001122334455','key':key},boot=778,id=1)
        new_token=resumed['body']['token'];self.assertNotEqual(token,new_token)
        self.assertEqual(self.packet('command',body,new_token,id=2,boot=778)['body']['code'],'accepted')
        self.assertIsNone(self.packet('auth.resume',{'peer':'001122334455','key':key},boot=777,id=102))
    def test_udp_pair_scope_is_enforced_and_persisted(self):
        for f in (1,2):
            self.mp('R')
            paired=self.packet('auth.pair',{'peer':'001122334455','password':'secret','scopes':2},f=f)
            key=paired['body']['key']
            for restart in (False,True):
                if restart:self.mp('X')
                resumed=self.packet('auth.resume',{'peer':'001122334455','key':key},id=2,f=f)
                token=resumed['body']['token']
                self.assertEqual(self.packet('read',token=token,id=3,f=f)['op'],'sample')
                self.assertIsNone(self.packet('command',{'request_id':'scope-on','action':'outlet.set','params':{'on':True}},token,id=4,f=f))
                self.assertFalse(self.query()['power'])
    def test_calibration_and_persistence(self):
        r=self.command('config.set',{'section':'meter','values':{'voltage_coefficient':2.0,'current_coefficient':0.5,'power_coefficient':1.0}})
        self.assertEqual(r['code'],'accepted');self.assertTrue(self.query()['metering']['calibrated'])
        self.mp('X');self.assertEqual(self.query('config.get')['current_coefficient'],0.5)
        self.assertEqual(self.command('config.set',{'section':'meter','values':{'current_coefficient':0}})['code'],'invalid_params')
    @staticmethod
    def frame(state=0x55,flags=0x70,pulse=1,measures=(1000,1000,1000)):
        b=bytearray([state,0x5a])
        for param,meas in zip((122340,1000,122340),measures):b+=param.to_bytes(3,'big')+meas.to_bytes(3,'big')
        b+=bytes([flags|((pulse>>16)<<7)])+(pulse&65535).to_bytes(2,'big')
        b.append(sum(b[2:])&255);return b
    def meter(self,**kw):return self.mp('M '+self.frame(**kw).hex())['metering']
    def test_meter_frames_checksum_freshness(self):
        m=self.meter();self.assertTrue(m['valid']);self.assertAlmostEqual(m['voltage'],229.9992);self.assertEqual(m['current'],1)
        self.assertEqual(m['received_bytes'],24)
        self.assertFalse(m['calibrated']);self.assertEqual(m['energy_kwh'],0)
        bad=self.frame();bad[-1]^=1;m=self.mp('M '+bad.hex())['metering'];self.assertEqual(m['frames'],1)
        m=self.mp('M '+(b'noise'+self.frame(pulse=2)).hex())['metering'];self.assertEqual(m['frames'],2);self.assertGreater(m['bad_frames'],0)
        self.mp('T 15000');m=self.query()['metering'];self.assertFalse(m['valid']);self.assertIsNone(m['active_power'])
        m=self.meter(flags=0);self.assertFalse(m['valid'])
    def test_meter_energy_rollover_and_faults(self):
        self.meter(pulse=131071);self.mp('T 10050');m=self.meter(pulse=0)
        self.assertAlmostEqual(m['energy_kwh'],122340*1.88/3.6e12)
        self.mp('T 10100');m=self.meter(pulse=50000);self.assertEqual(m['discontinuities'],1)
        saved=m['energy_kwh'];self.assertEqual(self.command('indicator.set',{'enabled':False})['code'],'accepted')
        self.mp('X');self.assertAlmostEqual(self.query()['metering']['energy_kwh'],saved)
        m=self.meter(state=0xaa);self.assertFalse(m['valid'])
        m=self.meter(state=0xf2);self.assertTrue(m['valid']);self.assertEqual(m['active_power'],0)
        m=self.meter(measures=(0,1000,1000));self.assertIsNone(m['voltage'])
    def test_malformed_corpus(self):
        rng=random.Random(8032)
        for _ in range(1500):
            data=rng.randbytes(rng.randrange(1,129))
            for f in (1,2):self.call(f'V {f} {data.hex()}')
            self.call('U '+data.hex())
        self.assertFalse(self.query()['power'])

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('host',type=Path);args,rest=parser.parse_known_args()
    HOST=str(args.host.resolve());unittest.main(argv=[__file__]+rest,verbosity=2)

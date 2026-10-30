#!/usr/bin/env python3
"""Independent Python MessagePack/CBOR clients drive sanitized product C code."""
import argparse
import json
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
DIAGNOSTICS = {item['path']: item['id'] for item in json.loads(
    (Path(__file__).resolve().parents[2] / 'common/diagnostics-schema.json').read_text()
)['fields']}

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
    def diagnostics(self):
        reply=self.query('diagnostics.get')
        self.assertEqual(reply['schema'],1)
        values=dict(reply['fields'])
        self.assertEqual(len(values),len(reply['fields']))
        return {path:values[id] for path,id in DIAGNOSTICS.items() if id in values}
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
    def test_power_on_off_and_restore_survive_reboots_and_failed_save(self):
        for mode,expected in [('on',True),('off',False),('restore',False)]:
            self.assertEqual(self.command('config.set',{'section':'device','values':{'power_on_mode':mode}},rid=mode)['code'],'accepted')
            self.mp('X');self.assertEqual(self.query()['power'],expected);self.assertEqual(self.query()['power_on_mode'],mode)
        self.assertEqual(self.command('outlet.set',{'on':True},rid='on-restore')['code'],'accepted')
        self.mp('X');self.assertTrue(self.query()['power'])
        self.mp('F 1');self.assertEqual(self.command('outlet.set',{'on':False},rid='off-failed')['code'],'storage_error')
        self.assertTrue(self.query()['power']);self.mp('F 0');self.mp('X');self.assertTrue(self.query()['power'])
        self.assertEqual(self.command('outlet.set',{'on':False},rid='off-restored')['code'],'accepted')
        self.mp('X');self.assertFalse(self.query()['power'])
        for values in ({'power_on_mode':'invalid'},{'power_on_mode':True},{'power_on_mode':'on','power_on':False}):
            self.assertEqual(self.command('config.set',{'section':'device','values':values},rid=str(values))['code'],'invalid_params')
    def power_rule(self,**kw):
        p=dict(enabled=True,trigger='power',comparison='above',power_milliwatts=100000,seconds=2,action='outlet.set',params={'on':True})
        p.update(kw)
        return p
    def poll_power(self,at,**sample):
        self.mp(f'T {at}');self.meter(**sample);return self.mp('P')
    def test_power_rules_monotonic_continuity_once_busy_and_manual_priority(self):
        result=self.command('schedules.put',self.power_rule());self.assertEqual(result['code'],'accepted');id=result['data']['id']
        self.assertFalse(self.poll_power(10000)['relay']);self.assertFalse(self.poll_power(11000)['relay'])
        self.assertTrue(self.poll_power(12000)['relay']);writes=self.mp('S')['writes']
        self.poll_power(13000);self.assertEqual(self.mp('S')['writes'],writes)
        # Missing/stale meter data breaks a pending duration; it does not rearm a fired rule.
        self.mp('T 20000');self.mp('P');self.poll_power(21000);self.assertEqual(self.mp('S')['writes'],writes)
        # Physical control wins even though the original condition still holds.
        self.mp('L');self.assertFalse(self.query()['power']);self.poll_power(22000);self.poll_power(23000);self.assertFalse(self.query()['power'])
        self.poll_power(24000,state=0xf2);self.poll_power(25000);self.poll_power(26000);self.assertTrue(self.poll_power(27000)['relay'])
        self.assertEqual(self.command('schedules.enable',{'id':id,'enabled':False},rid='disable')['code'],'accepted')
        self.assertEqual(self.command('outlet.set',{'on':False},rid='off')['code'],'accepted')
        self.poll_power(28000);self.assertFalse(self.query()['power'])
        # Rule replacement cancels the old timer. An interruption cannot count as sustained load.
        self.assertEqual(self.command('schedules.put',self.power_rule(id=id),rid='replace')['code'],'accepted')
        self.poll_power(29000);self.mp('T 35000');self.mp('P');self.poll_power(36000);self.poll_power(37000);self.assertTrue(self.poll_power(38000)['relay'])
        # Immediate below-threshold action retries a transient storage error without latching success.
        self.assertEqual(self.command('config.set',{'section':'device','values':{'power_on_mode':'restore'}},rid='policy')['code'],'accepted')
        self.assertEqual(self.command('schedules.put',self.power_rule(id=id,comparison='below',power_milliwatts=1,seconds=0,params={'on':False}),rid='below')['code'],'accepted')
        self.mp('F 1');self.assertTrue(self.poll_power(39000,state=0xf2)['relay'])
        self.mp('F 0');self.assertFalse(self.poll_power(40000,state=0xf2)['relay'])
        self.mp('X');self.assertFalse(self.query()['power'])
        listed=self.command('schedules.get',{},rid='list')['data'];self.assertEqual(listed['count'],1);self.assertEqual(listed['items'][0]['comparison'],'below')
    def test_power_rule_validation_capacity_and_persistence_rollback(self):
        for changes in ({'comparison':'equal'},{'seconds':True},{'seconds':604801},{'power_milliwatts':-1},{'power_milliwatts':25000001},{'action':'system.reboot'},{'params':{'toggle':True}},{'params':{'index':1,'on':False}},{'trigger':'weekly'}):
            self.assertEqual(self.command('schedules.put',self.power_rule(**changes),rid=str(changes))['code'],'invalid_params')
        self.mp('F 1');self.assertEqual(self.command('schedules.put',self.power_rule(),rid='failed')['code'],'storage_error');self.mp('F 0')
        for i in range(8):self.assertEqual(self.command('schedules.put',self.power_rule(seconds=i),rid='rule-'+str(i))['code'],'accepted')
        self.assertEqual(self.command('schedules.put',self.power_rule(),rid='overflow')['code'],'capacity')
        self.mp('X');items=self.command('schedules.get',{},rid='get')['data']['items'];self.assertEqual(len(items),8)
        self.mp('F 1');self.assertEqual(self.command('schedules.delete',{'id':items[0]['id']},rid='delete-failed')['code'],'storage_error');self.mp('F 0')
        self.assertEqual(self.command('schedules.delete',{'id':items[0]['id']},rid='delete')['code'],'accepted')
        self.assertEqual(self.command('schedules.put',self.power_rule(),rid='reuse')['code'],'accepted')
    def test_idempotency_and_conflict(self):
        first=self.command('config.set',{'section':'device','values':{'name':'Desk'}},i=1)
        before=self.mp('S');second=self.command('config.set',{'values':{'name':'Desk'},'section':'device'},i=2,o=2)
        self.assertEqual(first,second);self.assertEqual(before,self.mp('S'))
        self.assertEqual(self.command('config.set',{'section':'device','values':{'name':'Other'}})['code'],'conflict')
        self.assertEqual(self.command('config.set',{'section':'device','values':{'name':'Other'}},rid='new')['code'],'accepted')
        self.assertEqual(self.query('config.get')['name'],'Other')
    def test_name_set_is_bounded_atomic_and_replay_safe(self):
        self.assertIn('name.set',self.query('capabilities.get')['actions'])
        self.assertEqual(self.query('capabilities.get')['name_max_bytes'],63)
        before=self.query('config.get')
        for i,params in enumerate(({},dict(name=''),dict(name='x'*64),dict(name=True),dict(name='ok',power_on=True),dict(name='ok',password='bad'))):
            self.assertEqual(self.command('name.set',params,rid='bad-name-'+str(i))['code'],'invalid_params')
            self.assertEqual(self.query('config.get'),before)
        self.assertEqual(self.command('name.set',dict(name='Desk'),admin=False)['code'],'unauthorized')
        self.mp('F 1')
        self.assertEqual(self.command('name.set',dict(name='Desk'),rid='failed-name')['code'],'storage_error')
        self.assertEqual(self.query('config.get'),before)
        self.mp('F 0')
        first=self.command('name.set',dict(name='客厅 Desk'),rid='new-name')
        self.assertEqual(first['code'],'accepted');self.assertEqual(first['data']['name'],'客厅 Desk')
        counters=self.mp('S')
        self.assertEqual(first,self.command('name.set',dict(name='客厅 Desk'),rid='new-name',i=2,o=2))
        self.assertEqual(counters,self.mp('S'))
        self.assertEqual(self.command('name.set',dict(name='Other'),rid='new-name')['code'],'conflict')
        self.mp('X');self.assertEqual(self.query('config.get')['name'],'客厅 Desk')
        self.assertEqual(self.query('config.get')['socket_names'],before['socket_names'])
    def test_direct_query_response_parity(self):
        self.assertEqual(self.command('config.set',{'section':'device','values':{'name':'N'*63}},rid='long-name')['code'],'accepted')
        self.meter()
        for action in ('status.get','config.get','capabilities.get','info.get','power.get','diagnostics.get'):
            if action=='power.get':
                status=self.query();meter=status['metering']
                expected={'on':status['power'],'metering_supported':True,'power_watts':meter['active_power'],'energy_kwh':meter['energy_kwh'],'metering':meter}
            else:expected=self.query(action)
            for i in (1,2):
                for o in (1,2):
                    reply=self.command(action,{},rid='direct-query',i=i,o=o)
                    self.assertEqual(reply['request_id'],'direct-query')
                    self.assertEqual(reply['code'],'ok')
                    self.assertEqual(reply['data'],expected)
        for f in (1,2):
            reply=self.command('status.get',{'unknown':0},i=f,o=f)
            self.assertEqual(reply['code'],'invalid_params');self.assertEqual(reply['data'],{})
            reply=self.command('status.get',{},i=f,o=f,auth=False)
            self.assertEqual(reply['code'],'unauthorized');self.assertEqual(reply['data'],{})
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
        for params in ({'on':1},{'on':'true'},{'on':b'\0\xff'},{'on':True,'index':1},{'on':True,'unknown':0}):
            self.assertEqual(self.command('outlet.set',params,rid='bad-'+str(len(str(params))))['code'],'invalid_params')
        self.assertFalse(self.query()['power'])
        for f,raw in [(0,b'{}'),(1,b'\x82\xa1a\xc3\xa1a\xc2'),(2,b'\xa2\x61a\xf5\x61a\xf4'),(1,b'\x81\xa1a\xcb'+bytes.fromhex('7ff0000000000000')),(2,b'\xbf\xff'),(1,b'\x81\xa1a\xa1\xff')]:
            self.assertEqual(self.call(f'V {f} {raw.hex()}'),b'\0')
        for f in (1,2):
            for obj in ({'k':None},{'k':True},{'k':b'\0\xff'},{'k':-9007199254740991},{'k':9007199254740991},{'文本':'电量'}):
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
    def test_http_session_capacity_and_expired_slot_reuse(self):
        tokens=[]
        for i in range(4):
            self.mp('T '+str(10000+i*100));tokens.append(self.call('H secret').decode())
        self.mp('T 10500');self.assertEqual(self.call('B '+tokens[0]),b'\1')
        self.assertEqual(self.call('H wrong'),b'')
        self.assertTrue(all(self.call('B '+t)==b'\1' for t in tokens))
        # A busy login must retain even the least recently used active slot.
        self.mp('T 10600')
        for t in (tokens[0],tokens[2],tokens[3]):self.assertEqual(self.call('B '+t),b'\1')
        self.assertEqual(self.call('H secret'),b'')
        self.assertTrue(all(self.call('B '+t)==b'\1' for t in tokens))
        self.mp('T 1811000')
        self.assertTrue(all(self.call('B '+t)==b'\0' for t in tokens))
        fresh=self.call('H secret').decode();self.assertEqual(len(fresh),32)
        self.assertEqual(self.call('B '+fresh),b'\1')
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
        config=self.query('config.get')
        self.assertEqual([config[k+'_coefficient'] for k in ('voltage','current','power')],
                         [1.894288,1.0102,1.88835008906368])
        r=self.command('config.set',{'section':'meter','values':{'voltage_coefficient':2.0,'current_coefficient':0.5,'power_coefficient':1.0}})
        self.assertEqual(r['code'],'accepted');self.assertTrue(self.query()['metering']['calibrated'])
        self.mp('X');self.assertEqual(self.query('config.get')['current_coefficient'],0.5)
        self.assertEqual(self.command('config.set',{'section':'meter','values':{'current_coefficient':0}})['code'],'invalid_params')
    def test_precise_coefficients_rescale_and_atomic_save(self):
        before=self.meter()
        values={'voltage_coefficient':1.894288,'current_coefficient':1.0102,'power_coefficient':1.88835008906368}
        for i,f in enumerate((1,2)):
            self.meter()
            values['current_coefficient']=1.01023456789+i
            result=self.command('config.set',{'section':'meter','values':values},rid=f'precise-{i}',i=f,o=f)
            self.assertEqual(result['code'],'accepted')
            self.assertAlmostEqual(self.query()['metering']['current'],values['current_coefficient'])
            self.mp('X');config=self.query('config.get',f=f)
            self.assertEqual({k:config[k] for k in values},values)
        self.mp('F 1')
        self.assertEqual(self.command('config.set',{'section':'meter','values':{'current_coefficient':3}},rid='failed-save')['code'],'storage_error')
        self.assertEqual(self.query()['current_coefficient'],values['current_coefficient'])
        self.mp('X');self.assertEqual(self.query()['current_coefficient'],values['current_coefficient'])
        for i,value in enumerate((0,0.0009,100.001,float('nan'),float('inf'),-float('inf'),'1.01',True)):
            self.assertEqual(self.command('config.set',{'section':'meter','values':{'voltage_coefficient':value}},rid=f'bad-{i}')['code'],'invalid_params')
        self.assertTrue(before['valid'])
    @staticmethod
    def frame(state=0x55,flags=0x70,pulse=1,measures=(1000,1000,1000)):
        b=bytearray([state,0x5a])
        for param,meas in zip((122340,1000,122340),measures):b+=param.to_bytes(3,'big')+meas.to_bytes(3,'big')
        b+=bytes([flags|((pulse>>16)<<7)])+(pulse&65535).to_bytes(2,'big')
        b.append(sum(b[2:])&255);return b
    def meter(self,**kw):return self.mp('M '+self.frame(**kw).hex())['metering']
    def test_meter_frames_checksum_freshness(self):
        m=self.meter();self.assertTrue(m['valid']);self.assertAlmostEqual(m['voltage'],231.74719392);self.assertEqual(m['current'],1.0102)
        self.assertNotIn('received_bytes',m)
        self.assertEqual(self.diagnostics()['metering.received_bytes'],24)
        self.assertFalse(m['calibrated']);self.assertEqual(m['energy_kwh'],0)
        bad=self.frame();bad[-1]^=1;self.mp('M '+bad.hex());self.assertEqual(self.diagnostics()['metering.frames'],1)
        self.mp('M '+(b'noise'+self.frame(pulse=2)).hex());diag=self.diagnostics();self.assertEqual(diag['metering.frames'],2);self.assertGreater(diag['metering.bad_frames'],0)
        self.mp('T 15000');m=self.query()['metering'];self.assertFalse(m['valid']);self.assertIsNone(m['active_power'])
        m=self.meter(flags=0);self.assertFalse(m['valid'])
    def test_meter_energy_rollover_and_faults(self):
        self.meter(pulse=131071);self.mp('T 10050');m=self.meter(pulse=0)
        self.assertAlmostEqual(m['energy_kwh'],122340*1.88835008906368/3.6e12)
        self.mp('T 10100');m=self.meter(pulse=50000);self.assertEqual(self.diagnostics()['metering.discontinuities'],1)
        saved=m['energy_kwh'];self.assertEqual(self.command('indicator.set',{'enabled':False})['code'],'accepted')
        self.mp('X');self.assertAlmostEqual(self.query()['metering']['energy_kwh'],saved)
        m=self.meter(state=0xaa);self.assertFalse(m['valid'])
        m=self.meter(state=0xf2);self.assertTrue(m['valid']);self.assertEqual(m['active_power'],0)
        m=self.meter(measures=(0,1000,1000));self.assertIsNone(m['voltage'])
    def test_business_cache_hits_mutations_and_meter_expiry(self):
        initial=self.query();builds=self.diagnostics()['status_cache_builds']
        self.mp('T 10100')
        self.assertEqual(self.query(),initial)
        self.assertEqual(self.query(f=2),initial)
        self.assertEqual(self.diagnostics()['status_cache_builds'],builds)
        self.assertNotIn('uptime_seconds',initial)
        self.assertEqual(self.command('outlet.set',{'on':True})['code'],'accepted')
        self.assertTrue(self.query()['power'])
        self.assertGreater(self.diagnostics()['status_cache_builds'],builds)
        self.assertTrue(self.meter()['valid'])
        fresh=self.query();builds=self.diagnostics()['status_cache_builds']
        self.mp('T 10200');self.assertEqual(self.query(f=2),fresh)
        self.assertEqual(self.diagnostics()['status_cache_builds'],builds)
        self.mp('T 15100')
        expired=self.query();self.assertFalse(expired['metering']['valid'])
        self.assertIsNone(expired['metering']['active_power'])
        self.assertGreater(self.diagnostics()['status_cache_builds'],builds)
        self.assertEqual(self.query(f=2),expired)
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

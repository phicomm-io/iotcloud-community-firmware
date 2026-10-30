"""Real product callbacks/parser/scheduler with SDK-only boundaries stubbed.

Auth itself executes in test_auth_routes_mp. No JSON allocator/parser is linked.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
FIXTURE=ROOT/'tests/http_core'
SOAK_ENV='PH_HTTP_CALLBACK_SOAK_ROUNDS'
SOAK_DEFAULT=300
SOAK_MAX=100000

def soak_rounds():
 try: rounds=int(os.environ.get(SOAK_ENV,str(SOAK_DEFAULT)))
 except ValueError as exc: raise ValueError(f'{SOAK_ENV} must be an integer') from exc
 if not 1<=rounds<=SOAK_MAX: raise ValueError(f'{SOAK_ENV} must be between 1 and {SOAK_MAX}')
 return rounds

A1_EXTRA=r'''
 assert(start("POST","/api/v1/light/test",sizeof(post))==PH_HTTP_READ_BODY);assert(app.body(&x,post,sizeof(post)));app.complete(&x);assert(x.response.status==400);finish(true);
 {const unsigned char light[]={0x82,0xa4,'s','l','o','t',5,0xa9,'n','e','i','g','h','b','o','r','s',0xc3};
 assert(start("POST","/api/v1/light/test",sizeof(light))==PH_HTTP_READ_BODY);assert(app.body(&x,light,sizeof(light)));app.complete(&x);assert(x.response.status==200&&a1_state.light_test_slot==5&&a1_state.light_test_neighbors);finish(true);}
'''
M1_EXTRA=r'''
 unsigned char frame[12]={0x23,0x70,0,0,0,0,0,0,0,0,0,0x21}, packed[64];ph_buf b;ph_writer writer;
 phw_init_fixed(&writer,PH_WIRE_MSGPACK,&b,packed,sizeof(packed));phw_begin(&writer,NULL,false);phw_bytes_value(&writer,"frame",frame,sizeof(frame));phw_end(&writer);size_t frame_n=phw_finish_fixed(&writer);assert(frame_n);
 assert(start_auth("POST","/api/v1/mcu/exchange",frame_n,"Bearer bad")==PH_HTTP_REPLY&&x.response.status==401&&!mcu_calls);finish(false);
 assert(start("POST","/api/v1/mcu/exchange",5)==PH_HTTP_REPLY&&x.response.status==400);finish(false);
 assert(start("POST","/api/v1/mcu/exchange",289)==PH_HTTP_REPLY&&x.response.status==400);finish(false);
 assert(start("POST","/api/v1/mcu/exchange",frame_n)==PH_HTTP_READ_BODY);assert(app.body(&x,packed,3)&&app.body(&x,packed+3,frame_n-3));app.complete(&x);mem=parsed(200);const void*reply;size_t reply_n;
 assert(phv_get(&mem,"data",&data)&&phv_get(&data,"reply",&v)&&phv_bytes(&v,&reply,&reply_n)&&reply_n==20&&((const unsigned char*)reply)[19]==0x21&&mcu_calls==1);finish(true);
 assert(start("POST","/api/v1/mcu/exchange",frame_n)==PH_HTTP_READ_BODY);assert(app.body(&x,packed,3));app.complete(&x);assert(x.response.status==400&&mcu_calls==1);finish(false);
 mcu_fail=true;assert(start("POST","/api/v1/mcu/exchange",frame_n)==PH_HTTP_READ_BODY);assert(app.body(&x,packed,frame_n));app.complete(&x);assert(x.response.status==409&&mcu_calls==2);finish(true);mcu_fail=false;
 unsigned char image[164]={0};uint32_t sp=0x20008000,pc=0x4041;memcpy(image,&sp,4);memcpy(image+4,&pc,4);unsigned before_calls=mcu_calls;
 assert(start("POST","/api/v1/mcu/ota",163)==PH_HTTP_REPLY&&x.response.status==400);finish(false);assert(start("POST","/api/v1/mcu/ota",32769)==PH_HTTP_REPLY&&x.response.status==400);finish(false);
 m1_state.mcu_updating=true;assert(start("POST","/api/v1/mcu/ota",164)==PH_HTTP_REPLY&&x.response.status==409);finish(false);m1_state.mcu_updating=false;
 assert(start("POST","/api/v1/mcu/ota",164)==PH_HTTP_READ_BODY);assert(app.body(&x,image,3));finish(false);assert(mcu_calls==before_calls);
 image[0]=1;assert(start("POST","/api/v1/mcu/ota",164)==PH_HTTP_READ_BODY);assert(app.body(&x,image,164));app.complete(&x);assert(x.response.status==409&&mcu_calls==before_calls);finish(true);image[0]=0;
 uint32_t bad_pc=0xffffffffu;memcpy(image+4,&bad_pc,4);assert(start("POST","/api/v1/mcu/ota",164)==PH_HTTP_READ_BODY);assert(app.body(&x,image,164));app.complete(&x);assert(x.response.status==409&&mcu_calls==before_calls);finish(true);memcpy(image+4,&pc,4);
 assert(start("POST","/api/v1/mcu/ota",164)==PH_HTTP_READ_BODY);assert(app.body(&x,image,1)&&app.body(&x,image+1,163));app.complete(&x);assert(x.response.status==202&&mcu_commits==1&&mcu_received==164);finish(true);
 mcu_fail=true;assert(start("POST","/api/v1/mcu/ota",164)==PH_HTTP_READ_BODY);assert(app.body(&x,image,164));app.complete(&x);assert(x.response.status==409&&mcu_commits==1&&!m1_state.mcu_updating);finish(true);mcu_fail=false;
'''
TC1_EXTRA=r'''
 assert(start("GET","/api/v1/status?view=full",0)==PH_HTTP_REPLY);action("status.get");finish(true);assert(start("GET","/api/v1/status///",0)==PH_HTTP_REPLY);action("status.get");finish(true);
 assert(start("HEAD","/api/v1/status?probe=1",0)==PH_HTTP_REPLY&&x.response.suppress_body);finish(true);assert(start("POST","/api/v1/command///",sizeof(post))==PH_HTTP_READ_BODY);finish(false);
 unsigned before_reboots=reboots,before_activations=activations;
 assert(start("POST","/api/v1/ota/upload",3)==PH_HTTP_WAIT);assert(app.retry(&x)==PH_HTTP_READ_BODY);assert(app.body(&x,"123",3));app.complete(&x);assert(x.response.status==202&&job&&activations==before_activations);finish(false);assert(!job&&reboots==before_reboots&&activations==before_activations);
 wire_fail_send=true;execute_wire("POST /api/v1/ota/upload HTTP/1.1\r\nAuthorization: Bearer valid\r\nContent-Length: 3\r\n\r\n123");wire_fail_send=false;assert(wire_server_running&&!job&&reboots==before_reboots&&activations==before_activations);
 assert(start("POST","/api/v1/ota/upload",3)==PH_HTTP_WAIT);assert(app.retry(&x)==PH_HTTP_READ_BODY);assert(app.body(&x,"123",3));app.complete(&x);finish(true);assert(!job&&reboots==before_reboots+1&&activations==before_activations+1);
'''
TC1_FINAL=r'''
 unsigned final_reboots=reboots,final_activations=activations;fail_activation=true;
 assert(start("POST","/api/v1/ota/upload",3)==PH_HTTP_WAIT);assert(app.retry(&x)==PH_HTTP_READ_BODY);assert(app.body(&x,"123",3));app.complete(&x);finish(true);assert(!job&&reboots==final_reboots&&activations==final_activations+1);
 final_activations=activations;activation_uncertain=true;assert(start("POST","/api/v1/ota/upload",3)==PH_HTTP_WAIT);assert(app.retry(&x)==PH_HTTP_READ_BODY);assert(app.body(&x,"123",3));app.complete(&x);finish(true);assert(job&&reboots==final_reboots&&activations==final_activations+1);
 assert(start("POST","/api/v1/ota/upload",3)==PH_HTTP_REPLY&&x.response.status==503);finish(false);assert(job);assert(start("GET","/api/v1/status",0)==PH_HTTP_REPLY);action("status.get");finish(true);
 job=fail_activation=activation_uncertain=false;final_activations=activations;wire_fail_close=true;execute_wire("POST /api/v1/ota/upload HTTP/1.1\r\nAuthorization: Bearer valid\r\nContent-Length: 3\r\n\r\n123");assert(!wire_server_running&&!job&&activations==final_activations);wire_fail_close=false;
'''
WRITE_FAIL=r'''
 assert(start("POST","/api/v1/ota/upload",3)==PH_HTTP_WAIT);assert(app.retry(&x)==PH_HTTP_READ_BODY);fail_write=true;unsigned before_aborts=aborts;assert(!app.body(&x,"123",3));assert(aborts==before_aborts+1&&!job);job=true;finish(false);assert(job&&aborts==before_aborts+1);job=false;fail_write=false;aborts=0;
'''

class Callbacks(unittest.TestCase):
 def test_product_callbacks(self):
  rounds=soak_rounds()
  for model,slots in (('A1',3),('A1',4),('M1',4),('TC1',3),('TC1',4)):
   with self.subTest(model=model,slots=slots),tempfile.TemporaryDirectory() as temp:
    temp=Path(temp)
    source=(ROOT/(model+'/http.c' if model!='TC1' else 'TC1/http_server/app_httpd.c')).read_text()
    route=source[source.index('#define HTTP_PENDING_CLIENTS' if model!='TC1' else 'static ph_http_product product;'):]
    route=route.replace('#include "../common/httpd/local_auth.inc"','').replace('#include "../../common/httpd/local_auth.inc"','').replace('../../common/httpd/memory_mp.inc','common/httpd/memory_mp.inc').replace('../common/httpd/memory_mp.inc','common/httpd/memory_mp.inc')
    if model!='TC1':
     name=model.lower();stubs=(FIXTURE/'mico_mp.inc').read_text().replace('MODEL',model).replace('app_',name+'_')
     stubs+=(FIXTURE/('mcu_mp.inc' if model=='M1' else 'light_mp.inc')).read_text()
     stubs+=f'\n#define {model}_WEB_ETAG "\\\"page\\\""\n#define {model}_FAVICON_ETAG "\\\"icon\\\""\nstatic const unsigned char {name}_web_data[]={{31,139,1,2}},{name}_favicon_data[]={{1,2}};\n'
    else:stubs=(FIXTURE/'tc1_mp.inc').read_text()
    main=(FIXTURE/'callbacks_mp_main.inc').read_text().replace('MODEL_START','AppHttpdStart' if model=='TC1' else model.lower()+'_http_start').replace('MODEL_EXTRA',{'A1':A1_EXTRA+WRITE_FAIL,'M1':M1_EXTRA+WRITE_FAIL,'TC1':TC1_EXTRA}[model]).replace('MODEL_FINAL',TC1_FINAL if model=='TC1' else '')
    probe=temp/'probe.c';probe.write_text((FIXTURE/'callbacks_mp.c').read_text()+stubs+route+main)
    cmd=[os.environ.get('CC','cc'),'-DPH_HTTP_CLIENT_SLOTS='+str(slots),'-std=gnu99','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(ROOT),'-I'+str(ROOT/'tests/stubs'),str(probe),*[str(ROOT/p) for p in ('common/httpd/product.c','common/mqtt/diagnostics.c','common/httpd/parser.c','common/httpd/core.c','common/typed_wire.c','common/wire_buffer.c')],'-lm','-o',str(temp/'probe')]
    result=subprocess.run(cmd,capture_output=True,text=True,timeout=120)
    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
    result=subprocess.run([temp/'probe',str(rounds)],capture_output=True,text=True,timeout=max(60,min(900,30+rounds//250)))
    print(model,slots,result.stdout,flush=True)
    self.assertEqual(result.returncode,0,result.stdout+'\n'+result.stderr)
    self.assertIn(f'mixed read-only callback soak: {rounds} rounds',result.stdout)
if __name__=='__main__':unittest.main()

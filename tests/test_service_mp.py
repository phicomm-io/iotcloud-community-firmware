"""Actual dispatcher regressions; no device SDK or same-TU weak overrides.

Set NATIVE_MP_ROOT for a separate stable source checkout. Ordinary fixtures
compile its complete service.c as a separate TU; the OTA fixture includes its
unchanged service_mp.inc with a small job backend, testing dispatch gates rather
than the platform OTA worker or transport. Recovery overrides live in hooks.c.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
SOURCE=Path(os.environ.get('NATIVE_MP_ROOT',ROOT))

class NativeService(unittest.TestCase):
 def run_c(self,body,runs=1,ota=False):
  with tempfile.TemporaryDirectory(prefix='service-mp-') as temporary:
   t=Path(temporary);(t/'generated').mkdir();(t/'generated/build_ota.h').write_text('')
   (t/'case.c').write_text((ROOT/'tests/service_mp/fixture.inc').read_text()+body)
   cmd=shlex.split(os.environ.get('CC','cc'))+['-std=gnu99','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-Wno-misleading-indentation','-Wno-unused-function','-Wno-unused-variable','-g','-I'+str(ROOT/'tests/service_mp'),'-I'+str(t),'-I'+str(SOURCE),'-pthread']
   if ota:cmd+=['-DPHICOMM_DEVICE_OTA=1']
   if os.environ.get('SANITIZE')=='1':cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
   cmd += [str(t/'case.c'),str(ROOT/'tests/service_mp/ota_dispatcher.c' if ota else SOURCE/'common/service.c'),str(ROOT/'tests/service_mp/hooks.c'),str(SOURCE/'common/typed_wire.c'),str(SOURCE/'common/wire_buffer.c'),'-Wl,--wrap=malloc','-Wl,--wrap=free','-lcrypto','-lm','-o',str(t/'case')]
   built=subprocess.run(cmd,capture_output=True,text=True,timeout=30);self.assertEqual(built.returncode,0,built.stderr)
   for run in range(runs):
    ran=subprocess.run([str(t/'case'),str(run)],capture_output=True,text=True,timeout=30);self.assertEqual(ran.returncode,0,f'run={run}\n'+ran.stderr)

 def test_real_recovery_gate_and_full_reset_exception(self):
  self.run_c(r'''
int main(void){
 service_init();device_config_recovery_set(true);assert(device_config_recovery());
 command_code(request("fan.set","blocked",-1),"migration_required");assert(!mutations);
 command_code(request("system.reset","empty",-1),"migration_required");assert(!mutations);
 command_code(request("system.reset","false",0),"migration_required");assert(!mutations);
 command_code(request("system.reset","full",1),"accepted");assert(mutations==1);
 command_code(request("system.reboot","reboot",-1),"accepted");command_code(request("wifi.scan","scan",-1),"accepted");assert(mutations==3);
 ph_buf *out=service_query_mp("status.get");code(out,"ok");ph_buf_free(out);
 device_config_recovery_set(false);assert(!device_config_recovery());command_code(request("fan.set","ready",-1),"accepted");assert(mutations==4);return 0;
}
''')

 def test_receipt_oom_never_repeats_executed_mutation(self):
  self.run_c(r'''
int main(int argc,char **argv){
 (void)argc;service_init();ph_buf *q=request("fan.set","once",-1);
 allocation_budget=strtol(argv[1],NULL,10);ph_buf *out=service_command_mp(q->buf,q->len);allocation_budget=-1;ph_buf_free(out);
 unsigned before=mutations;assert(before<=1);out=service_command_mp(q->buf,q->len);assert(out);assert(mutations==1);
 if(!before)code(out,"accepted");ph_buf_free(out);
 out=service_command_mp(q->buf,q->len);assert(out);assert(mutations==1);ph_buf_free(out);ph_buf_free(q);
 command_code(request("system.reset","once",1),"conflict");assert(mutations==1);return 0;
}
''',runs=12)

 def test_cache_event_during_capture_requires_followup_capture(self):
  self.run_c(r'''
int main(void){
 service_init();change_during_capture=true;ph_buf *a=service_query_mp("status.get");code(a,"ok");assert(captures==1);ph_buf_free(a);
 a=service_query_mp("status.get");code(a,"ok");assert(captures==2);ph_buf_free(a);
 a=service_query_mp("status.get");code(a,"ok");assert(captures==2);ph_buf_free(a);
 a=service_query_mp("capabilities.get");code(a,"ok");ph_buf_free(a);assert(caps==1);
 service_cache_changed(SERVICE_CACHE_STATUS);a=service_query_mp("capabilities.get");code(a,"ok");ph_buf_free(a);assert(caps==1);
 a=service_query_mp("status.get");code(a,"ok");ph_buf_free(a);assert(captures==3);
 service_event("info","system","boot");a=service_query_mp("status.get");code(a,"ok");ph_buf_free(a);assert(captures==4);return 0;
}
''')

 def test_mqtt_only_session_route_and_input_lifetime(self):
  self.run_c(r'''
int main(void){
 service_init();ph_writer w;assert(phw_init(&w,1));phw_begin(&w,NULL,false);phw_text(&w,"action","sensor.session.set");phw_text(&w,"request_id","session");phw_begin(&w,"params",false);phw_text(&w,"session","independent-copy");phw_end(&w);phw_end(&w);ph_buf *q=phw_take(&w);assert(q);
 ph_buf *out=service_command_mp(q->buf,q->len);code(out,"unsupported");assert(!mqtt_calls);ph_buf_free(out);
 out=service_mqtt_command_mp(q->buf,q->len);code(out,"accepted");assert(mqtt_calls==1);memset(q->buf,0,q->len);ph_buf_free(q);
 assert(!strcmp(async_session,"independent-copy"));code(out,"accepted");ph_buf_free(out);assert(!mutations);return 0;
}
''')

 def test_busy_capture_recovery_and_independent_cached_responses(self):
  self.run_c(r'''
int main(void){
 service_init();fail_capture=true;ph_buf *a=service_query_mp("status.get");code(a,"busy");ph_buf_free(a);assert(captures==1);
 a=service_query_mp("status.get");code(a,"ok");ph_buf *b=service_query_mp("status.get");code(b,"ok");assert(captures==2&&a->buf!=b->buf);
 memset(a->buf,0,a->len);code(b,"ok");ph_buf_free(a);ph_buf_free(b);return 0;
}
''')

 def test_malformed_requests_ids_and_status_parameters(self):
  self.run_c(r'''
static ph_buf *bad_request(unsigned variant){
 ph_writer w;assert(phw_init(&w,1));phw_begin(&w,NULL,false);
 phw_text(&w,"action","status.get");
 if(variant==0)phw_text(&w,"unknown","x");
 if(variant==1)phw_text(&w,"","x");
 if(variant==2)phw_text(&w,"action||params","x");
 if(variant==3)phw_text(&w,"request_id","");
 if(variant==4)phw_text(&w,"request_id","bad/id");
 if(variant==5)phw_int(&w,"request_id",1);
 if(variant==6)phw_text(&w,"request_id","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
 if(variant==7)phw_bool(&w,"params",true);
 else if(variant!=8){phw_begin(&w,"params",false);
  if(variant==9)phw_bool(&w,"view",true);
  if(variant==10)phw_text(&w,"view","invalid");
  if(variant==11)phw_text(&w,"extra","unknown");phw_end(&w);}
 phw_end(&w);return phw_take(&w);
}
int main(void){
 service_init();for(unsigned variant=0;variant<12;variant++)command_code(bad_request(variant),"invalid_params");assert(!captures&&!mutations);
 const char json[]="{\"action\":\"fan.set\",\"params\":{}}";ph_buf *out=service_command_mp(json,sizeof(json)-1);code(out,"invalid_params");ph_buf_free(out);
 const unsigned char duplicate[]={0x83,0xa6,'a','c','t','i','o','n',0xa8,'f','a','n','.','s','e','t','!',0xa6,'p','a','r','a','m','s',0x80,0xa6,'p','a','r','a','m','s',0x80};
 out=service_command_mp(duplicate,sizeof(duplicate));code(out,"invalid_params");ph_buf_free(out);assert(!mutations);return 0;
}
''')

 def test_concurrent_duplicate_requests_execute_once(self):
  self.run_c(r'''
#include <pthread.h>
static ph_buf *shared_request;
static void *send_once(void *arg){
 (void)arg;ph_buf *out=service_command_mp(shared_request->buf,shared_request->len);code(out,"accepted");ph_buf_free(out);return NULL;
}
int main(void){
 service_init();shared_request=request("fan.set","concurrent-once",-1);
 pthread_t readers[4];for(unsigned i=0;i<4;i++)assert(!pthread_create(&readers[i],NULL,send_once,NULL));
 for(unsigned i=0;i<4;i++)assert(!pthread_join(readers[i],NULL));assert(mutations==1);ph_buf_free(shared_request);return 0;
}
''')

 def test_known_query_unknown_params_are_invalid(self):
  self.run_c(r'''
int main(void){
 service_init();const char *actions[]={"info.get","capabilities.get","ota.status.get","ota.cancel"};
 for(unsigned i=0;i<4;i++){
  ph_writer w;assert(phw_init(&w,1));phw_begin(&w,NULL,false);phw_text(&w,"action",actions[i]);phw_begin(&w,"params",false);phw_bool(&w,"unknown",true);phw_end(&w);phw_end(&w);
  command_code(phw_take(&w),"invalid_params");
 }assert(!mutations&&!captures&&!caps);return 0;
}
''')

 def test_ota_active_gating_status_validation_and_cancel_recovery(self):
  self.run_c(r'''
extern bool test_ota_active;
int main(void){
 service_init();ph_buf *out=service_query_mp("ota.status.get");code(out,"ok");ph_value data=data_value(out),active;bool yes;
 assert(phv_get(&data,"active",&active)&&phv_bool(&active,&yes)&&!yes);ph_buf_free(out);
 test_ota_active=true;command_code(request("fan.set","blocked-by-ota",-1),"busy");assert(!mutations);
 ph_writer w;assert(phw_init(&w,1));phw_begin(&w,NULL,false);phw_text(&w,"action","status.get");phw_begin(&w,"params",false);phw_text(&w,"view","invalid");phw_end(&w);phw_end(&w);command_code(phw_take(&w),"invalid_params");
 out=service_query_mp("status.get");code(out,"ok");data=data_value(out);assert(phv_get(&data,"active",&active)&&phv_bool(&active,&yes)&&yes);assert(!captures);ph_buf_free(out);
 out=service_query_mp("capabilities.get");code(out,"ok");assert(caps_restricted);ph_buf_free(out);
 command_code(request("ota.cancel","cancel",-1),"accepted");assert(!test_ota_active);
 out=service_query_mp("status.get");code(out,"ok");assert(captures==1);ph_buf_free(out);
 command_code(request("fan.set","blocked-by-ota",-1),"accepted");assert(mutations==1);
 command_code(request("fan.set","blocked-by-ota",-1),"accepted");assert(mutations==1);return 0;
}
''',ota=True)

if __name__=='__main__':unittest.main()

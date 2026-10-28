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
 def run_c(self,body,runs=1,ota=False,ilp32=False,worker_prefix=None):
  with tempfile.TemporaryDirectory(prefix='service-mp-') as temporary:
   t=Path(temporary);(t/'generated').mkdir();(t/'generated/build_ota.h').write_text('')
   (t/'version.h').write_text('extern const char *test_local_version;\n#define FIRMWARE_VERSION test_local_version\n')
   (t/'case.c').write_text((ROOT/'tests/service_mp/fixture.inc').read_text()+body)
   cmd=shlex.split(os.environ.get('CC','cc'))+['-std=gnu99','-DMOC=1','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-Wno-misleading-indentation','-Wno-unused-function','-Wno-unused-variable','-g','-I'+str(ROOT/'tests/service_mp'),'-I'+str(t),'-I'+str(SOURCE),'-pthread']
   if ota or worker_prefix:cmd+=['-DPHICOMM_DEVICE_OTA=1']
   if worker_prefix:cmd+=['-DTEST_ACTUAL_OTA_WORKER=1','-DFEATURE_DEVICE_A1=1','-include',str(t/'version.h'),'-DPHICOMM_OTA_METADATA_PREFIX="'+worker_prefix+'"',str(ROOT/'tests/service_mp/ota_worker_hooks.c')]
   if ilp32:cmd+=['-DTEST_ILP32_STRTOL=1','-Wl,--wrap=strtol']
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

 def test_clock_values_expire_status_but_keep_unchanged_capabilities(self):
  self.run_c(r'''
int main(void){
 service_init();ph_buf *out=service_query_mp("status.get");code(out,"ok");ph_buf_free(out);assert(captures==1);
 out=service_query_mp("capabilities.get");code(out,"ok");ph_buf_free(out);assert(caps==1);
 test_clock_ms+=999;out=service_query_mp("status.get");ph_buf_free(out);assert(captures==1);
 test_clock_ms++;out=service_query_mp("status.get");code(out,"ok");ph_buf_free(out);assert(captures==2);
 out=service_query_mp("capabilities.get");code(out,"ok");ph_buf_free(out);assert(caps==1);
 service_cache_changed(SERVICE_CACHE_STATUS);out=service_query_mp("status.get");ph_buf_free(out);assert(captures==3);return 0;
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

 def test_logs_paging_redaction_dictionary_boundaries_and_bad_types(self):
  body=r'''
#include <errno.h>
static ph_buf *logs(int64_t after,int variant){
 ph_writer w;assert(phw_init(&w,1));phw_begin(&w,NULL,false);phw_text(&w,"action","logs.get");phw_begin(&w,"params",false);
 if(after>=0)phw_int(&w,"after",after);
 if(variant==0)phw_int(&w,"limit",8);
 if(variant==1)phw_int(&w,"limit",9);
 if(variant==2)phw_int(&w,"limit",0);
 if(variant==3)phw_int(&w,"limit",-1);
 if(variant==4)phw_bool(&w,"limit",true);
 if(variant==5)phw_text(&w,"limit","8");
 if(variant==6)phw_number(&w,"limit",1.5);
 if(variant==7)phw_bool(&w,"after",false);
 if(variant==8)phw_text(&w,"after","1");
 if(variant==9)phw_int(&w,"unknown",1);
 phw_end(&w);phw_end(&w);return phw_take(&w);
}
static int64_t integer(const ph_value *v,const char *key){ph_value n;int64_t result;assert(phv_get(v,key,&n)&&phv_int(&n,&result));return result;}
static ph_buf *run_logs(int64_t after,int variant,const char *expected){ph_buf *q=logs(after,variant),*out=service_command_mp(q->buf,q->len);ph_buf_free(q);code(out,expected);return out;}
int main(void){
 service_init();errno=ERANGE;ph_buf_free(run_logs(-1,0,"ok"));
 for(int variant=1;variant<=9;variant++)ph_buf_free(run_logs(-1,variant,"invalid_params"));
 ph_buf_free(run_logs(INT64_C(2147483648),0,"invalid_params"));
 /* 64-entry ring: the oldest eight requested records are returned, never a
  * secret/URL/payload copied into a diagnostic string. */
 for(unsigned i=0;i<70;i++)service_event("info","test","transition");
 service_event("info","test","password=secret");service_event("info","test","https://secret.invalid/path");
 ph_buf *out=run_logs(1,0,"ok");ph_value data=data_value(out),records,flag,row,event;bool overwritten;
 assert(phv_get(&data,"overwritten",&flag)&&phv_bool(&flag,&overwritten)&&overwritten);
 assert(phv_get(&data,"records",&records)&&records.kind==PHV_ARRAY&&records.count==8);
 assert(integer(&data,"next")==integer(&data,"latest")-56);
 for(unsigned i=0;i<8;i++){assert(phv_at(&records,i,&row)&&phv_get(&row,"event",&event)&&phv_equal_text(&event,"transition"));}
 int64_t next=integer(&data,"next");ph_buf_free(out);
 out=run_logs(next,0,"ok");data=data_value(out);assert(phv_get(&data,"records",&records)&&records.count==8);assert(integer(&data,"next")==next+8);ph_buf_free(out);
 out=run_logs(-1,-1,"ok");data=data_value(out);int64_t latest=integer(&data,"latest");ph_buf_free(out);
 errno=ERANGE;service_event("warn","mqtt","receive_error_-2147483648");service_event("warn","mqtt","subscribe_error_2147483647");
 service_event("warn","mqtt","receive_error_-2147483649");service_event("warn","mqtt","subscribe_error_2147483648");service_event("info","test","unregistered_event");
 out=run_logs(latest,-1,"ok");data=data_value(out);assert(phv_get(&data,"records",&records)&&records.count==2);
 assert(phv_at(&records,0,&row)&&phv_get(&row,"event",&event)&&phv_equal_text(&event,"receive_error_-2147483648"));
 assert(phv_at(&records,1,&row)&&phv_get(&row,"event",&event)&&phv_equal_text(&event,"subscribe_error_2147483647"));
 assert(integer(&data,"latest")==latest+2);ph_buf_free(out);return 0;
}
'''
  self.run_c(body)
  self.run_c(body,ilp32=True)

 def test_ten_thousand_owned_cleanup_cycles_keep_only_fixed_caches(self):
  self.run_c(r'''
int main(void){
 service_init();
 const char *ids[]={"retained-0","retained-1","retained-2","retained-3","retained-4","retained-5","retained-6","retained-7"};
 for(unsigned i=0;i<8;i++)command_code(request("fan.set",ids[i],-1),"accepted");
 const char *actions[]={"status.get","capabilities.get","info.get"};
 for(unsigned i=0;i<3;i++){ph_buf *out=service_query_mp(actions[i]);code(out,"ok");ph_buf_free(out);}
 size_t baseline_blocks=live_blocks,baseline_bytes=live_bytes;
 assert(baseline_blocks==20);/* eight receipts + two caches, descriptor/bytes each */
 for(unsigned i=0;i<10000;i++){
  ph_buf *out=service_query_mp(actions[i%3]);code(out,"ok");ph_buf_free(out);
  assert(live_blocks==baseline_blocks&&live_bytes==baseline_bytes);
  assert(total_allocations-total_frees==live_blocks);
  service_memory_state_t m;service_memory_snapshot(&m);assert(!m.active&&!m.reserved);
 }
 assert(captures==1&&caps==1&&total_allocations>20000);return 0;
}
''')

 def test_large_binary_params_replay_digest_and_no_retained_input(self):
  self.run_c(r'''
#include <stdio.h>
static ph_buf *large_request(unsigned i,bool changed){
 unsigned char payload[7000];memset(payload,0x41,sizeof(payload));if(changed)payload[6999]=0x42;
 char id[32];snprintf(id,sizeof(id),"large-%u",i);ph_writer w;assert(phw_init(&w,1));
 phw_begin(&w,NULL,false);phw_text(&w,"action","large.set");phw_text(&w,"request_id",id);phw_begin(&w,"params",false);phw_bytes_value(&w,"payload",payload,sizeof(payload));phw_end(&w);phw_end(&w);return phw_take(&w);
}
int main(void){
 service_init();size_t retained_bytes=0;
 for(unsigned i=0;i<8;i++){
  ph_buf *q=large_request(i,false);assert(q&&q->len>7000&&q->len<8192);
  ph_buf *out=service_command_mp(q->buf,q->len);code(out,"accepted");
  retained_bytes+=sizeof(ph_buf)+out->len+1;ph_buf_free(out);
  out=service_command_mp(q->buf,q->len);code(out,"accepted");ph_buf_free(out);assert(large_calls==i+1);
  /* Poison and free the borrowed request: only a digest and small response
   * can remain. Exact accounting would catch even one retained 7 KiB copy. */
  memset(q->buf,0,q->len);ph_buf_free(q);
  assert(live_blocks==2*(i+1)&&live_bytes==retained_bytes&&live_bytes<2048);
 }
 command_code(large_request(7,true),"conflict");assert(large_calls==8&&mutations==8);
 assert(live_blocks==16&&live_bytes==retained_bytes);
 ph_buf *out=service_query_mp("status.get");code(out,"ok");ph_buf_free(out);assert(live_blocks==18);
 assert(total_allocations-total_frees==live_blocks);return 0;
}
''')

 def test_live_heap_pressure_incomplete_output_and_post_mutation_tombstone(self):
  self.run_c(r'''
int main(void){
 service_init();test_heap_free=64000;service_memory_state_t before,after;
 service_memory_snapshot(&before);allocation_budget=0;assert(!service_query_mp("status.get"));allocation_budget=-1;
 service_memory_snapshot(&after);assert(after.wire_alloc_failed>before.wire_alloc_failed&&after.wire_guard_denied==before.wire_guard_denied);
 before=after;exhaust_capture=true;
 /* Real MOC heap checks reject growth after the first fields were emitted;
  * neither a truncated map nor an apparently successful envelope escapes. */
 ph_buf *out=service_query_mp("status.get");assert(!out&&captures==1&&!live_blocks);
 test_heap_free=SERVICE_MEMORY_RESERVE;assert(!service_query_mp("status.get"));assert(!live_blocks);
 service_memory_snapshot(&after);assert(after.wire_guard_denied>before.wire_guard_denied&&after.wire_alloc_failed==before.wire_alloc_failed);
 test_heap_free=64000;out=service_query_mp("status.get");code(out,"ok");ph_buf_free(out);size_t cached=live_blocks,cached_bytes=live_bytes;assert(cached==2);
 ph_buf *q=request("fan.set","incomplete-after-mutation",-1);exhaust_mutation=true;
 out=service_command_mp(q->buf,q->len);assert(!out&&mutations==1);
 test_heap_free=64000;out=service_command_mp(q->buf,q->len);code(out,"busy");assert(mutations==1);ph_buf_free(out);ph_buf_free(q);
 assert(live_blocks==cached&&live_bytes==cached_bytes);service_memory_state_t m;service_memory_snapshot(&m);assert(!m.active&&!m.reserved);return 0;
}
''')

 def test_live_byte_admission_sixty_four_claims_and_diagnostics(self):
  self.run_c(r'''
int main(void){
 service_init();test_heap_free=64000;service_memory_state_t m;
 service_memory_mqtt(7,false,900);service_memory_snapshot(&m);assert(m.mqtt_pending==7&&m.mqtt_peak==7&&m.mqtt_max_delay==900);
 service_memory_mqtt(0,true,50);service_memory_snapshot(&m);assert(!m.mqtt_pending&&m.mqtt_peak==7&&m.mqtt_overflow==1&&m.mqtt_max_delay==900);
 service_memory_waiting(true);service_memory_waiting(true);service_memory_waiting(false);service_memory_snapshot(&m);assert(m.waiting==1);service_memory_waiting(false);service_memory_waiting(false);
 unsigned claims[64];for(unsigned i=0;i<64;i++){claims[i]=service_memory_claim(0);assert(claims[i]==SERVICE_MEMORY_WORK);}
 service_memory_snapshot(&m);assert(m.active==64&&m.reserved==64*SERVICE_MEMORY_WORK&&!m.denied&&!m.waiting);
 for(unsigned i=0;i<64;i++)service_memory_release(claims[i]);service_memory_snapshot(&m);assert(!m.active&&!m.reserved);
 test_heap_free=SERVICE_MEMORY_RESERVE+511;assert(!service_memory_claim(0));service_memory_snapshot(&m);assert(m.denied==1&&m.minimum_free==SERVICE_MEMORY_RESERVE+511);
 test_heap_free=SERVICE_MEMORY_RESERVE+SERVICE_MEMORY_WORK+100;unsigned claim=service_memory_claim(100);assert(claim==SERVICE_MEMORY_WORK+100);assert(!service_memory_claim(0));service_memory_release(claim);
 test_heap_free=64000;assert(!service_memory_claim(16385));service_memory_snapshot(&m);assert(m.denied==3&&!m.active&&!m.reserved);
 /* Admission recovers with the same request once live bytes return. */
 claim=service_memory_claim(100);assert(claim);service_memory_release(claim);service_memory_snapshot(&m);assert(!m.active&&!m.reserved&&m.denied==3);return 0;
}
''')

 def test_embedded_nul_null_params_and_trailing_bytes_are_rejected(self):
  self.run_c(r'''
int main(void){
 service_init();ph_buf *q=request("status.get",NULL,-1);
 /* A text token with an embedded NUL cannot alias another action. */
 char *text=NULL;for(size_t i=0;i+10<=q->len;i++)if(!memcmp(q->buf+i,"status.get",10)){text=q->buf+i;break;}assert(text);text[6]=0;
 ph_buf *out=service_command_mp(q->buf,q->len);code(out,"invalid_params");ph_buf_free(out);ph_buf_free(q);
 q=request("status.get",NULL,-1);char extra[256];assert(q->len+1<sizeof(extra));memcpy(extra,q->buf,q->len);extra[q->len]=0;
 out=service_command_mp(extra,q->len+1);code(out,"invalid_params");ph_buf_free(out);ph_buf_free(q);
 ph_writer w;assert(phw_init(&w,1));phw_begin(&w,NULL,false);phw_text(&w,"action","fan.set");phw_null(&w,"params");phw_end(&w);
 command_code(phw_take(&w),"invalid_params");assert(!mutations&&!captures);return 0;
}
''')

 def test_explicit_receipt_copy_oom_and_bounded_eviction(self):
  self.run_c(r'''
#include <stdio.h>
int main(int argc,char **argv){
 (void)argc;service_init();ph_buf *q=request("fan.set","receipt-oom",-1);
 /* Four allocations produce payload+envelope. The next two allocations are
  * the saved response descriptor/bytes; exercise both independent failures. */
 allocation_budget=4+strtol(argv[1],NULL,10);ph_buf *out=service_command_mp(q->buf,q->len);allocation_budget=-1;
 code(out,"accepted");ph_buf_free(out);assert(mutations==1&&live_blocks==2);/* q still owns two blocks */
 out=service_command_mp(q->buf,q->len);code(out,"busy");ph_buf_free(out);assert(mutations==1);ph_buf_free(q);assert(!live_blocks);
 size_t retained_bytes=0;
 for(unsigned i=0;i<100;i++){
  char id[32];snprintf(id,sizeof(id),"evict-%03u",i);q=request("fan.set",id,-1);out=service_command_mp(q->buf,q->len);code(out,"accepted");
  size_t one=sizeof(ph_buf)+out->len+1;ph_buf_free(out);ph_buf_free(q);
  if(i<8)retained_bytes+=one;
  assert(live_blocks==2*(i<8?i+1:8)&&live_bytes==retained_bytes);
  assert(total_allocations-total_frees==live_blocks);
 }
 assert(mutations==101);command_code(request("fan.set","evict-099",-1),"accepted");assert(mutations==101);
 command_code(request("fan.set","evict-000",-1),"accepted");assert(mutations==102&&live_blocks==16&&live_bytes==retained_bytes);return 0;
}
''',runs=2)

class OtaMetadataWorkerService(unittest.TestCase):
 run_c=NativeService.run_c
 def test_real_worker_channel_version_gate_and_signed_install_path(self):
  golden=(ROOT/'tests/release_manifest_mp/publisher.msgpack').read_bytes()
  body='\nstatic const unsigned char publisher_golden[]={'+','.join(map(str,golden))+'};\n'+r'''
#include "mico.h"
extern ph_buf *test_manifest;
extern bool test_fetch_ok;
extern unsigned test_fetch_calls;
extern char test_fetch_url[512];
static void manifest(const char *version,bool signed_path){
 ph_value original,v;assert(phv_parse(1,publisher_golden,sizeof(publisher_golden),&original));
 ph_writer w;assert(phw_init(&w,1));phw_begin(&w,NULL,false);ph_iterator it;assert(phv_iter(&original,&it));char key[128],tag[64],path[160];
 snprintf(tag,sizeof(tag),"v%s",version);snprintf(path,sizeof(path),"/releases/v%s/A1.ota.bin%s",version,signed_path?".signed":"");
 while(phv_next(&it,key,&v)){
  if(!strcmp(key,"version"))phw_text(&w,key,version);
  else if(!strcmp(key,"tag"))phw_text(&w,key,tag);
  else if(!strcmp(key,"path"))phw_text(&w,key,path);
  else phw_value(&w,key,&v);
 }
 phw_end(&w);ph_buf_free(test_manifest);test_manifest=phw_take(&w);assert(test_manifest);
}
static void check(const char *source){
 ph_writer w;assert(phw_init(&w,1));phw_begin(&w,NULL,false);phw_text(&w,"action","ota.check");phw_begin(&w,"params",false);phw_text(&w,"source",source);phw_end(&w);phw_end(&w);
 ph_buf *q=phw_take(&w),*out=service_command_mp(q->buf,q->len);code(out,"accepted");ph_buf_free(out);
 memset(q->buf,0,q->len);ph_buf_free(q);/* worker must retain no borrowed input */
 test_run_worker();
}
static void state(const char *expected,bool available){
 ph_buf *out=service_query_mp("ota.check.get");code(out,"ok");ph_value d=data_value(out),v;bool yes;
 assert(phv_get(&d,"state",&v)&&phv_equal_text(&v,expected));assert(phv_get(&d,"available",&v)&&phv_bool(&v,&yes)&&yes==available);ph_buf_free(out);
}
int main(void){
 service_init();manifest("1.0.0-rc.5",true);check("github");state("available",true);
 assert(test_fetch_calls==1&&!strcmp(test_fetch_url,"https://phicomm-io.github.io" PHICOMM_OTA_METADATA_PREFIX "/a1/full/latest.msgpack"));
 command_code(request("ota.install",NULL,-1),"accepted");assert(installs==1&&!strcmp(installed_url,"https://phicomm-io.github.io/releases/v1.0.0-rc.5/A1.ota.bin.signed"));
 manifest("1.0.0-rc.4",true);check("github");state("up_to_date",false);command_code(request("ota.install",NULL,-1),"invalid_params");assert(installs==1);
 manifest("0.9.9",true);check("github");state("up_to_date",false);
 manifest("1.0.0",true);check("github");state("available",true);
 /* A failed selected source performs exactly one request. It never falls
  * back from candidate to stable or to another host, nor preserves install. */
 test_fetch_ok=false;unsigned before=test_fetch_calls;check("cn");state("failed",false);
 assert(test_fetch_calls==before+1&&!strcmp(test_fetch_url,"http://phicomm.fjrcn.cn" PHICOMM_OTA_METADATA_PREFIX "/a1/full/latest.msgpack"));
 command_code(request("ota.install",NULL,-1),"invalid_params");assert(installs==1);
 test_fetch_ok=true;manifest("1.0.1",false);check("global");state("failed",false);
 assert(!strcmp(test_fetch_url,"http://phicomm.fjrcn.com" PHICOMM_OTA_METADATA_PREFIX "/a1/full/latest.msgpack"));
 command_code(request("ota.install",NULL,-1),"invalid_params");assert(installs==1);ph_buf_free(test_manifest);test_manifest=NULL;
 assert(!live_blocks&&total_allocations==total_frees);return 0;
}
'''
  for prefix in ['/ota/candidate','/ota/v1']:
   with self.subTest(prefix=prefix):self.run_c(body,worker_prefix=prefix)

 def test_legacy_update_worker_matrix_and_admission(self):
  """All 16 old device-update cases through real service.c, now native MP."""
  golden=(ROOT/'tests/release_manifest_mp/publisher.msgpack').read_bytes()
  body='\nstatic const unsigned char golden[]={'+','.join(map(str,golden))+'};\n'+r'''
#include "mico.h"
#include "common/ota/release_manifest.h"
extern const char *test_local_version;
extern bool ota_job_active(void);
extern ph_buf *test_manifest;
extern unsigned test_fetch_calls;
static void manifest(const char *version,const char *model,const char *path){
 ph_value root,v;assert(phv_parse(1,golden,sizeof(golden),&root));ph_iterator it;assert(phv_iter(&root,&it));
 ph_writer w;assert(phw_init(&w,1));phw_begin(&w,NULL,false);char key[128],tag[80];snprintf(tag,sizeof(tag),"v%s",version);
 while(phv_next(&it,key,&v)){
  if(!strcmp(key,"version"))phw_text(&w,key,version);
  else if(!strcmp(key,"tag"))phw_text(&w,key,tag);
  else if(!strcmp(key,"model"))phw_text(&w,key,model);
  else if(!strcmp(key,"path"))phw_text(&w,key,path);
  else phw_value(&w,key,&v);
 }phw_end(&w);ph_buf_free(test_manifest);test_manifest=phw_take(&w);assert(test_manifest);
}
static ph_buf *check_request(const char *source){
 ph_writer w;assert(phw_init(&w,1));phw_begin(&w,NULL,false);phw_text(&w,"action","ota.check");phw_begin(&w,"params",false);
 if(source)phw_text(&w,"source",source);phw_end(&w);phw_end(&w);return phw_take(&w);
}
static void check(const char *remote,const char *model,const char *path,const char *expected){
 manifest(remote,model,path);unsigned before=test_fetch_calls,started=installs;
 command_code(check_request("cn"),"accepted");assert(ota_job_active());
 command_code(check_request("cn"),"busy");assert(test_fetch_calls==before);
 ph_buf *out=service_query_mp("ota.check.get");ph_value d=data_value(out),v;bool busy;
 assert(phv_get(&d,"busy",&v)&&phv_bool(&v,&busy)&&busy);ph_buf_free(out);
 test_run_worker();assert(!ota_job_active()&&test_fetch_calls==before+1);
 out=service_query_mp("ota.check.get");code(out,"ok");d=data_value(out);bool available;
 assert(phv_get(&d,"state",&v)&&phv_equal_text(&v,expected));
 assert(phv_get(&d,"busy",&v)&&phv_bool(&v,&busy)&&!busy);
 assert(phv_get(&d,"available",&v)&&phv_bool(&v,&available)&&available==!strcmp(expected,"available"));
 assert(phv_get(&d,"version",&v)&&phv_equal_text(&v,!strcmp(expected,"failed")?"":remote));ph_buf_free(out);
 command_code(request("ota.install",NULL,-1),available?"accepted":"invalid_params");assert(installs==started+(unsigned)available);
 if(available)assert(!strcmp(installed_url,"http://phicomm.fjrcn.cn/releases/v1.0.0/fw.bin.signed"));
}
int main(void){
 static const struct{const char *local,*remote,*state;} cases[]={
 {"1.0.0-rc-abcdef","1.0.0-123456","available"},
 {"1.0.0-abcdef","1.0.0-rc-123456","up_to_date"},
 {"1.0.0-rc.2-abcdef","1.0.0-rc.10-123456","available"},
 {"1.0.0-rc.10-abcdef","1.0.0-rc.2-123456","up_to_date"},
 {"1.0.0-rc-abcdef","1.0.0-rc.0","available"},
 {"1.0.0-abcdef-dirty","1.0.0+metadata.1","up_to_date"},
 {"1.0.0-rc.2+old","1.0.0-rc.2+new","up_to_date"},
 {"0.9.27-abcdef","1.0.0-rc-123456","available"},
 {"1.0.0-rc-abcdef","0.9.27-123456","up_to_date"},
 {"1.0.1-abcdef","1.0.0","up_to_date"},
 {"1.0.0-rc-abcdef","1.0.0-rc.01","failed"},
 {"1.0.0-rc-abcdef","1.0.0-rc-garbage","failed"},
 {"1.0.0-rc-abcdef","9.0.0-beta","failed"},
 {"1.0.0-rc-abcdef","01.0.0","failed"},
 {"1.0.0-rc-abcdef","4294967296.0.0","failed"},
 {"1.0.0-unknown","2.0.0","failed"}};
 ph_ota_version_t parsed;assert(ph_ota_version_parse("0.10.0",&parsed)&&parsed.core[1]==10);
 assert(!ph_ota_version_parse("latest",&parsed));assert(!ph_release_path("/releases/v1/../../escape"));
 service_init();command_code(check_request("missing"),"invalid_params");command_code(check_request(NULL),"invalid_params");command_code(check_request("invalid"),"invalid_params");assert(!ota_job_active()&&!test_fetch_calls);
 for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);i++){
  test_local_version=cases[i].local;check(cases[i].remote,"A1","/releases/v1.0.0/fw.bin.signed",cases[i].state);
 }
 test_local_version="1.0.0";
 check("9.10.0","A1","/releases/v1.0.0/fw.bin.signed","available");
 check("0.0.0","A1","/releases/v1.0.0/fw.bin.signed","up_to_date");
 check("9.10.0","TC1","/releases/v1.0.0/fw.bin.signed","failed");
 check("9.10.0","A1","http://evil.invalid/fw","failed");
 ph_buf_free(test_manifest);test_manifest=NULL;assert(!live_blocks&&total_allocations==total_frees);return 0;
}
'''
  self.run_c(body,worker_prefix='/ota/candidate')

class LanPairService(unittest.TestCase):
 run_c=NativeService.run_c
 def test_pairing_borrows_input_without_retaining_credentials_or_receipt(self):
  self.run_c(r'''
static unsigned pairs,last_scope;static char pair_password[33];
bool feature_lan_pair_start(const char *peer,const char *ip,const char *password,unsigned scopes,const char **code){
 assert(!strcmp(peer,"001122aabbcc")&&!strcmp(ip,"10.0.0.2")&&scopes>=1&&scopes<=7);
 strcpy(pair_password,password);last_scope=scopes;pairs++;*code="accepted";return true;
}
bool feature_lan_pair_status(const char *peer,ph_writer *w){
 if(strcmp(peer,"001122aabbcc"))return false;phw_begin(w,NULL,false);phw_text(w,"phase","paired");phw_end(w);return true;
}
static ph_buf *pair_request(const char *action,const char *peer,int scopes){
 ph_writer w;assert(phw_init(&w,1));phw_begin(&w,NULL,false);phw_text(&w,"action",action);phw_text(&w,"request_id","same");
 phw_begin(&w,"params",false);phw_text(&w,"peer",peer);
 if(!strcmp(action,"lan.pair")){phw_text(&w,"ip","10.0.0.2");phw_text(&w,"password","adminfjr");phw_int(&w,"scopes",scopes);}
 phw_end(&w);phw_end(&w);return phw_take(&w);
}
int main(void){
 service_init();command_code(pair_request("lan.pair","001122aabbcc",8),"invalid_params");command_code(pair_request("lan.pair","001122aabbcc",0),"invalid_params");assert(!pairs);
 ph_buf *q=pair_request("lan.pair","001122aabbcc",3),*out=service_command_mp(q->buf,q->len);code(out,"accepted");
 ph_value data=data_value(out),v;assert(!phv_get(&data,"password",&v));ph_buf_free(out);
 memset(q->buf,0,q->len);ph_buf_free(q);assert(pairs==1&&!strcmp(pair_password,"adminfjr"));
 command_code(pair_request("lan.pair","001122aabbcc",3),"accepted");assert(pairs==2);/* credentials deliberately never retained as receipts */
 for(unsigned scope=4;scope<=7;scope++){
  command_code(pair_request("lan.pair","001122aabbcc",scope),"accepted");assert(last_scope==scope);
 }
 assert(pairs==6);
 command_code(pair_request("lan.pair.get","001122aabbcc",0),"ok");
 command_code(pair_request("lan.pair.get","000000000000",0),"not_found");return 0;
}
''')

if __name__=='__main__':unittest.main()

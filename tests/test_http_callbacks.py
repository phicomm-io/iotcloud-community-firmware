"""Production product callbacks + real shared JSON lifetime helpers, ASan/UBSan."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
PREFIX=(ROOT/'tests/http_core/callbacks.c').read_text()
SOAK_ENV='PH_HTTP_CALLBACK_SOAK_ROUNDS'
SOAK_DEFAULT=300
SOAK_MAX=100000

def soak_rounds():
 try: rounds=int(os.environ.get(SOAK_ENV,str(SOAK_DEFAULT)))
 except ValueError as exc: raise ValueError(f'{SOAK_ENV} must be an integer') from exc
 if not 1<=rounds<=SOAK_MAX: raise ValueError(f'{SOAK_ENV} must be between 1 and {SOAK_MAX}')
 return rounds

class Callbacks(unittest.TestCase):
 def test_product_callbacks(self):
  rounds=soak_rounds()
  timeout=max(60,min(900,30+rounds//250))
  sdk=Path(os.environ.get('PHICOMM_TEST_JSON_DIR',ROOT/'build/a1-sdk/mico-os/libraries/utilities/json_c'))
  for model,slots in (('A1',3),('M1',4),('TC1',3),('TC1',4)):
   with self.subTest(model=model,slots=slots),tempfile.TemporaryDirectory() as temp:
    temp=Path(temp);json=temp/'json';shutil.copytree(sdk,json)
    for p in json.glob('*.c'):p.write_text(p.read_text().replace('%ld','%d'))
    source=(ROOT/(model+'/http.c' if model!='TC1' else 'TC1/http_server/app_httpd.c')).read_text()
    if model!='TC1':
     prefix=model.lower()
     globals=source[source.index('#define HTTP_PENDING_CLIENTS'):source.index('static void str(')]
     tail=source[source.index('static void memory_response('):]
     stubs=r'''
#define kNoErr 0
#define MODEL_JSON_LIMIT 8192
typedef struct {char web_password[33];} app_config_t;
static void app_config_get(app_config_t*c){strcpy(c->web_password,password);}
static bool app_ota_begin(uint32_t n){assert(n);ota_begins++;job=true;return true;}
static bool app_ota_begin_unsigned(uint32_t n){return app_ota_begin(n);}
static void app_ota_abort(const char*r);
static bool app_ota_write(const void*p,size_t n){assert(p&&n<=512);written+=n;if(fail_write){app_ota_abort("write failed");return false;}return true;}
static bool app_ota_finish(void){finishes++;job=false;return true;}
static void app_ota_abort(const char*r){assert(r);aborts++;job=false;}
#define OSStatus int
'''.replace('MODEL',model).replace('app_',prefix+'_')
     if model=='A1':
      stubs+='''
static unsigned a1_lock;
static struct {bool ota_active,reboot_pending,light_test_neighbors;unsigned light_test_slot,light_test_until;} a1_state;
static void mico_rtos_lock_mutex(unsigned*p){assert(!*p);*p=1;}
static void mico_rtos_unlock_mutex(unsigned*p){assert(*p);*p=0;}
'''
     stubs+=f'''\n#define {model}_WEB_ETAG "\\\"page\\\""\n#define {model}_FAVICON_ETAG "\\\"icon\\\""\nstatic const unsigned char {prefix}_web_data[]={{31,139,1,2}};\nstatic const unsigned char {prefix}_favicon_data[]={{1,2}};\n'''
    else:
     # This fixture tests transport/OTA callbacks. Typed status capture and
     # encoding have separate actual-SDK fixtures; exclude their wrappers.
     globals=source[source.index('static ph_http_product product;'):source.index('static __attribute__((noinline)) json_object *tc1_status_full(')]
     tail=source[source.index('static void memory_response('):]
     stubs=r'''
static bool tc1_http_authorized(const char*a){return ph_http_basic(a,"admin",password);}
static int get_last_reset_reason(void){return 1;}
static size_t tc1_http_trace_format(char*p,size_t n){assert(n>=3);memcpy(p,"{}",3);return 2;}
static bool ota_job_claim(void){if(job)return false;job=true;return true;}
static void ota_job_release(void){assert(job);job=false;}
static bool ota_job_enter(void){assert(job);entered++;return true;}
static bool tc1_ota_reserve(void){return true;}
static bool tc1_ota_begin(uint32_t n,bool rescue){assert(n);ota_begins++;(void)rescue;return true;}
static bool tc1_ota_write(const void*p,size_t n){assert(p&&n<=512);written+=n;return !fail_write;}
static bool tc1_ota_verify(void){finishes++;return true;}
static bool tc1_ota_activate(void){activations++;return !fail_activation;}
static bool tc1_ota_commit_uncertain(void){return activation_uncertain;}
static void tc1_ota_failure(const char*r){assert(r);aborts++;}
static void tc1_ota_abort(void){aborts++;}
static void *mico_system_context_get(void){return NULL;}
#define eState_Software_Reset 1
static void mico_system_power_perform(void*p,int state){(void)p;assert(state==1);reboots++;job=false;}
int ph_http_stop(void){return 0;}
static const unsigned char web_index_html[]={31,139,1,2};
'''.replace('static ph_http_diagnostics ph_http_snapshot','ph_http_diagnostics ph_http_snapshot')
    main=r'''
static ph_http_request r;
static ph_http_exchange x;
static ph_http_action start_auth(const char*method,const char*path,unsigned length,const char*auth){
 memset(&r,0,sizeof(r));strcpy(r.method,method);strcpy(r.path,path);r.have_length=true;r.length=length;
 if(auth)strncpy(r.authorization,auth,sizeof(r.authorization)-1);memset(&x,0,sizeof(x));x.request=&r;return app.begin(&x);
}
static ph_http_action start(const char*method,const char*path,unsigned length){return start_auth(method,path,length,"Basic YWRtaW46cHc=");}
static void finish(bool sent){app.release(&x,sent);assert(app.admission()==(job?2:PH_HTTP_CLIENT_SLOTS));assert(!claims&&!waiting&&!json_token);json_heap_stats_t stats;json_heap_snapshot(&stats);assert(!stats.live_blocks);}
static void assert_clean(void){json_heap_stats_t stats;assert(!claims&&!waiting&&!json_token);json_heap_snapshot(&stats);assert(!stats.live_blocks);}
static void assert_json_response(unsigned status,const char*needle){if(x.response.status!=status)fprintf(stderr,"response status %u expected %u body=%.*s\n",x.response.status,status,(int)x.response.length,(const char*)(x.response.body?x.response.body:(const void*)""));assert(x.response.status==status);assert(x.response.body&&x.response.length);assert(strstr(x.response.body,needle));}
static void execute_wire(const char*wire){
 ph_http_server server;wire_input=wire;wire_position=wire_size=0;wire_accepted=wire_closed=false;memset(wire_output,0,sizeof(wire_output));
 assert(ph_http_server_open(&server,&wire_io,&app,3));
 for(unsigned n=0;n<2000&&!wire_closed;n++)ph_http_server_step(&server);
 assert(wire_closed);wire_server_running=server.running;ph_http_server_close(&server);assert(!claims&&!waiting&&!json_token);
}
int main(int argc,char**argv){
 assert(argc==2);char*end=NULL;unsigned long rounds=strtoul(argv[1],&end,10);assert(end&&*end==0&&rounds>=1&&rounds<=100000);
 strcpy(password,"pw");assert(MODEL_START()==0);assert(app.admission()==PH_HTTP_CLIENT_SLOTS);
 execute_wire("GET / HTTP/1.1\r\nAuthorization: Basic YWRtaW46cHc=\r\n\r\n");assert(strstr(wire_output,"Content-Length: 4\r\n"));
 execute_wire("OPTIONS /api/v1/status HTTP/1.1\r\nOrigin: https://mqtt.fjrcn.cn\r\n\r\n");assert(strstr(wire_output,"200 OK")&&strstr(wire_output,"Access-Control-Allow-Origin: https://mqtt.fjrcn.cn"));
 fail_cap=true;execute_wire("POST /api/v1/ota/upload HTTP/1.1\r\nAuthorization: Basic YWRtaW46cHc=\r\nContent-Length: 3\r\n\r\n123");assert(!ota_begins&&!job);aborts=0;fail_cap=false;
 WRITE_FAILURE_CHECK
 HEAD_CHECK
 assert(start("GET","/api/v1/memory",0)==PH_HTTP_REPLY);assert(x.response.status==200&&!claims&&!product.wire);assert(strstr(x.response.body,"http_core_close_failures"));finish(true);
 memory.free_memory=memory.allocted_memory=memory.num_of_chunks=INT32_MAX;maximum=true;assert(start("GET","/api/v1/memory",0)==PH_HTTP_REPLY);assert(x.response.status==200&&x.response.length<sizeof(product.scalar));printf("maximum memory JSON: %zu bytes\n",x.response.length);json_object *diagnostic=json_tokener_parse(x.response.body);assert(diagnostic);json_object_put(diagnostic);finish(true);maximum=false;
 for(unsigned i=0;i<6;i++){const char *paths[]={"/api/v1/info","/api/v1/status","/api/v1/capabilities","/api/v1/config","/api/v1/logs","/api/v1/schedules"};assert(start("GET",paths[i],0)==PH_HTTP_REPLY);assert(x.response.status==200);finish(true);}
 assert(start("GET","/api/v1/status",0)==PH_HTTP_REPLY);assert(x.response.status==200&&claims&&product.wire);assert(strstr(x.response.body,"status.get"));finish(true);
 unsigned other=service_json_claim();assert(other);
 assert(start("GET","/api/v1/status",0)==PH_HTTP_WAIT);assert(waiting&&!claims&&json_token==other);
 app.release(&x,false);assert(!waiting&&!claims&&json_token==other);service_json_release(other);
 deny=true;assert(start("GET","/api/v1/status",0)==PH_HTTP_WAIT);assert(waiting&&!claims);deny=false;assert(app.retry(&x)==PH_HTTP_REPLY);assert(!waiting&&claims);finish(false);
 assert(start("POST","/api/v1/command",2)==PH_HTTP_READ_BODY);assert(product.body&&claims);assert(app.body(&x,"{}",2));app.complete(&x);assert(commands==1&&product.wire);finish(true);
 command_alias=true;assert(start("POST","/api/v1/command",20)==PH_HTTP_READ_BODY);assert(app.body(&x,"{\"alias\":\"retained\"}",20));app.complete(&x);assert(product.wire&&strstr(x.response.body,"retained"));finish(true);command_alias=false;
 assert(start("POST","/api/v1/command",10)==PH_HTTP_READ_BODY);assert(app.body(&x,"{",1));finish(false);
 wire_stall=true;execute_wire("POST /api/v1/command HTTP/1.1\r\nAuthorization: Basic YWRtaW46cHc=\r\nContent-Length: 2\r\n\r\n");wire_stall=false;assert_clean();assert(app.admission()==PH_HTTP_CLIENT_SLOTS);
 assert(start("POST","/api/v1/ota/upload",100)==OTA_ACTION);if(OTA_ACTION==PH_HTTP_WAIT)assert(app.retry(&x)==PH_HTTP_READ_BODY);assert(job&&app.admission()==2);assert(app.body(&x,"123",3));finish(false);assert(aborts==1&&!job);
 assert(start("POST","/api/v1/ota/upload-unsigned",3)==OTA_ACTION);if(OTA_ACTION==PH_HTTP_WAIT)assert(app.retry(&x)==PH_HTTP_READ_BODY);app.body(&x,"123",3);app.complete(&x);assert(x.response.status==202&&finishes==1);finish(true);assert(!job);
 TC1_VERIFY_RECOVERY_CHECK
 assert(start("GET","/",0)==PH_HTTP_REPLY);assert(x.response.status==200&&x.response.gzip&&x.response.body);ASSET_CHECK finish(true);
 strcpy(password,"other");assert(start("GET","/",0)==PH_HTTP_REPLY);assert(x.response.status==401&&x.response.realm);finish(true);
 assert(start("OPTIONS","/missing",0)==PH_HTTP_REPLY);assert(x.response.status==OPTIONS_UNKNOWN);finish(true);
 assert(start("GET","/legacy-unregistered",0)==PH_HTTP_REPLY);assert(x.response.status==404||x.response.status==401);finish(true);
 strcpy(password,"pw");
 for(unsigned long round=0;round<rounds;round++){
  assert(start("GET","/api/v1/status",0)==PH_HTTP_REPLY);assert_json_response(200,"status.get");finish(true);
  const char post[]="{\"action\":\"status.get\"}";
  assert(start("POST","/api/v1/command",sizeof(post)-1)==PH_HTTP_READ_BODY);assert(product.body&&app.body(&x,post,sizeof(post)-1));app.complete(&x);assert_json_response(200,"command");finish(true);
  assert(start("POST","/api/v1/command",32)==PH_HTTP_READ_BODY);assert(product.body&&app.body(&x,"{\"action\":",10));finish(false);
  deny=true;assert(start("GET","/api/v1/status",0)==PH_HTTP_WAIT);assert(waiting&&!claims);deny=false;assert(app.retry(&x)==PH_HTTP_REPLY);assert_json_response(200,"status.get");finish(false);
  assert(start_auth("GET","/api/v1/status",0,"Basic YWRtaW46YmFk") == PH_HTTP_REPLY);assert(x.response.status==401);finish(true);
  assert(start("GET","/api/v1/not-found",0)==PH_HTTP_REPLY);assert(x.response.status==404||x.response.status==403);finish(true);
  assert_clean();
 }
 printf("mixed read-only callback soak: %lu rounds; status GET/POST, truncated release, deny-retry, auth failure, unknown route\n",rounds);
 TC1_ACTIVATION_FAILURE_CHECK
 return 0;
}
'''.replace('TC1_VERIFY_RECOVERY_CHECK','' if model!='TC1' else r'''
 unsigned before_reboots=reboots,before_activations=activations;
 assert(start("POST","/api/v1/ota/upload",3)==PH_HTTP_WAIT);assert(app.retry(&x)==PH_HTTP_READ_BODY);assert(app.body(&x,"123",3));app.complete(&x);assert(x.response.status==202&&job&&activations==before_activations);finish(false);assert(!job&&reboots==before_reboots&&activations==before_activations);
 wire_fail_send=true;execute_wire("POST /api/v1/ota/upload HTTP/1.1\r\nAuthorization: Basic YWRtaW46cHc=\r\nContent-Length: 3\r\n\r\n123");wire_fail_send=false;assert(wire_server_running);assert(!job&&reboots==before_reboots&&activations==before_activations);
 execute_wire("GET /api/v1/status HTTP/1.1\r\nAuthorization: Basic YWRtaW46cHc=\r\n\r\n");assert(strstr(wire_output,"200 OK"));
 assert(start("POST","/api/v1/ota/upload",3)==PH_HTTP_WAIT);assert(app.retry(&x)==PH_HTTP_READ_BODY);assert(app.body(&x,"123",3));app.complete(&x);finish(true);assert(!job&&reboots==before_reboots+1&&activations==before_activations+1);
''').replace('TC1_ACTIVATION_FAILURE_CHECK','' if model!='TC1' else r'''
 strcpy(password,"pw");before_reboots=reboots;before_activations=activations;fail_activation=true;
 assert(start("POST","/api/v1/ota/upload",3)==PH_HTTP_WAIT);assert(app.retry(&x)==PH_HTTP_READ_BODY);assert(app.body(&x,"123",3));app.complete(&x);finish(true);assert(!job&&reboots==before_reboots&&activations==before_activations+1);
 before_activations=activations;activation_uncertain=true;
 assert(start("POST","/api/v1/ota/upload",3)==PH_HTTP_WAIT);assert(app.retry(&x)==PH_HTTP_READ_BODY);assert(app.body(&x,"123",3));app.complete(&x);finish(true);assert(job&&reboots==before_reboots&&activations==before_activations+1);
 assert(start("POST","/api/v1/ota/upload",3)==PH_HTTP_REPLY&&x.response.status==503);finish(false);assert(job&&reboots==before_reboots);
 assert(start("GET","/api/v1/status",0)==PH_HTTP_REPLY&&x.response.status==200);finish(true);assert(job);
 /* Independent fresh platform boundary: close failure is NOT recoverable. */
 job=fail_activation=activation_uncertain=false;before_reboots=reboots;before_activations=activations;wire_fail_close=true;
 execute_wire("POST /api/v1/ota/upload HTTP/1.1\r\nAuthorization: Basic YWRtaW46cHc=\r\nContent-Length: 3\r\n\r\n123");
 assert(!wire_server_running&&!job&&reboots==before_reboots&&activations==before_activations);wire_fail_close=false;
''').replace('MODEL_START','AppHttpdStart' if model=='TC1' else model.lower()+'_http_start').replace('WRITE_FAILURE_CHECK','' if model=='TC1' else 'assert(start("POST","/api/v1/ota/upload",3)==PH_HTTP_WAIT);assert(app.retry(&x)==PH_HTTP_READ_BODY);fail_write=true;unsigned before_aborts=aborts;assert(!app.body(&x,"123",3));assert(aborts==before_aborts+1&&!job);job=true;finish(false);assert(job&&aborts==before_aborts+1);job=false;fail_write=false;aborts=0;').replace('OTA_ACTION','PH_HTTP_WAIT').replace('OPTIONS_UNKNOWN','404' if model=='TC1' else '403').replace('HEAD_CHECK','assert(start("GET","/api/v1/status?view=full",0)==PH_HTTP_REPLY&&x.response.status==200);finish(true);assert(start("GET","/api/v1/status///",0)==PH_HTTP_REPLY&&x.response.status==200);finish(true);assert(start("HEAD","/api/v1/status?probe=1",0)==PH_HTTP_REPLY&&x.response.status==200&&x.response.suppress_body);finish(true);assert(start("POST","/api/v1/command///",2)==PH_HTTP_READ_BODY);finish(false);assert(start("GET","/api/v1/statusx",0)==PH_HTTP_REPLY&&x.response.status==404);finish(true);assert(start("GET","/assetsunknown",0)==PH_HTTP_REPLY&&x.response.status==404);finish(true);execute_wire("HEAD / HTTP/1.1\\r\\nAuthorization: Basic YWRtaW46cHc=\\r\\n\\r\\n");assert(strstr(wire_output,"Content-Length: 4\\r\\n"));assert(!strcmp(strstr(wire_output,"\\r\\n\\r\\n")+4,""));' if model=='TC1' else '').replace('ASSET_CHECK','assert(x.response.private_cache&&x.response.etag);strcpy(r.if_none_match,"W/\\\"page\\\"");assert(app.begin(&x)==PH_HTTP_REPLY&&x.response.status==304);' if model!='TC1' else '')
    probe=temp/'probe.c';probe.write_text(PREFIX+stubs+globals+tail+main)
    cmd=[os.environ.get('CC','cc'),'-DPH_HTTP_CLIENT_SLOTS='+str(slots),'-DPH_HTTP_SCALAR_BYTES='+('1536' if model=='TC1' else '1280'),'-std=gnu99','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(ROOT),'-I'+str(ROOT/'tests/stubs'),'-I'+str(json),str(probe),str(ROOT/'common/httpd/product.c'),str(ROOT/'common/json_owned.c'),str(ROOT/'common/httpd/parser.c'),str(ROOT/'common/httpd/core.c'),*map(str,json.glob('*.c')),'-o',str(temp/'probe')]
    subprocess.run(cmd,check=True,timeout=120)
    result=subprocess.run([temp/'probe',str(rounds)],capture_output=True,text=True,timeout=timeout)
    self.assertEqual(result.returncode,0,result.stdout+'\n'+result.stderr)
    self.assertIn(f'mixed read-only callback soak: {rounds} rounds',result.stdout)
if __name__=='__main__':unittest.main()

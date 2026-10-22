"""Actual A1 status graph + pinned SDK JSON/ARM32 nano memory regression.

The ledger models each native allocation as align8(request)+8 bytes and counts
copy-before-free realloc peaks. It is not a MiCO heap emulator or a guarantee
against concurrent closed SDK allocations/fragmentation. No guard is lowered.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from tests.sdk_json import copy_patched_json

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def status_fixture(dual=False):
    source = (ROOT/'A1/http.c').read_text()
    builders = source[source.index('static void str('):source.index('static void memory_response(')]
    watchdog = '\n'.join(function((ROOT/'common/watchdog.c').read_text(), f'void watchdog_status_{suffix}(') for suffix in ('capture','encode','json'))
    filter_fn = function((ROOT/'common/service_product.inc').read_text(), 'json_object *a1_filter_json(')
    snapshots = r'''
#include "common/watchdog.h"
static bool enabled=true,failed,startup_complete=true;
static const char *fault="";static int reset_raw=0x30;
static struct {bool seen;} points[WATCHDOG_SLOT_COUNT]={{true},{true},{true},{true}};
static uint32_t watchdog_lock(void){return 0;}
static void watchdog_unlock(uint32_t n){(void)n;}
static const char *reset_name(void){return "software";}
void a1_filter_get(a1_filter_storage_t *p){memset(p,0,sizeof(*p));p->lifetime_hours=2000;}
'''
    capture = function((ROOT/'A1/main.c').read_text(), 'void a1_status_capture(')
    snapshots += r'''
mico_mutex_t a1_config_lock,a1_lock;
OSStatus mico_rtos_lock_mutex(mico_mutex_t*m){(void)m;return kNoErr;}
OSStatus mico_rtos_unlock_mutex(mico_mutex_t*m){(void)m;return kNoErr;}
void feature_values_snapshot(feature_values_t*out){memset(out,0,sizeof(*out));}
void feature_fan_snapshot(bool*eo,unsigned*es,bool*automatic,unsigned*limit){feature_effective_fan(cfg.fan_on,cfg.speed,eo,es);*automatic=false;*limit=100;}
'''
    builders = '#define PHICOMM_APPLICATION_WATCHDOG 1\n#define ph_http_snapshot http_diag_snapshot\n#include "tests/http_core/status_reference.c"\n#define a1_config (&cfg)\n#define a1_state state\n'+capture+'\n#undef a1_config\n#undef a1_state\n'+builders+'\n#undef ph_http_snapshot\n'
    service = (ROOT/'common/service.c').read_text()
    result = function(service, 'static json_object *result(')
    query = function(service, 'json_object *service_query(')
    result = 'static void string(json_object*o,const char*k,const char*v){json_build_object_add(o,k,json_object_new_string(v));}\n' + result
    result += r'''
json_object *service_command(json_object *request){
 const char *id=NULL;json_object *rid=json_object_object_get(request,"request_id");
 if(rid)id=json_object_get_string(rid);
 return result(id,"ok",a1_status_json());
}
'''
    mqtt=(ROOT/'tests/http_core/status_mqtt_reference.c').read_text()
    wire=function(mqtt,'static struct printbuf *service_mqtt_wire(').replace('service_mqtt_wire(', 'production_mqtt_wire(',1)
    mqtt_fixture=r'''
#define MQTT_SUCCESS 0
#define MQTT_FAILURE -1
#include "common/mqtt/owner.h"
static int mqtt_resource_result;
int ph_mqtt_resource_ready(void){return mqtt_resource_result;}
static bool snapshot_deferred;static uint32_t snapshot_retry_at;
static bool mqtt_modern,service_mqtt_capabilities_pending;static unsigned mqtt_bytes,service_mqtt_reply_count;
static int service_mqtt_flush(int (*sender)(const char*,const char*,bool)){(void)sender;return MQTT_SUCCESS;}
''' + wire + r'''
json_object *service_product_capabilities(void){return json_object_new_object();}
static struct printbuf *service_mqtt_wire(json_object *o){
 if(mqtt_modern)return production_mqtt_wire(o);
 struct printbuf *wire=o?json_object_to_json_string_ex(o):NULL;json_object_put(o);return wire;
}
static int mqtt_sender(const char *topic,const char *wire,bool retain){
 assert(retain&&strstr(topic,"/state"));assert(strstr(wire,"\"model\": \"a1\""));mqtt_bytes=strlen(wire);return MQTT_SUCCESS;
}
''' + function(mqtt,'static int snapshot_resource_wait(') + '\n' + function(mqtt,'static int service_mqtt_publish(') + '\n' + function(mqtt,'static int service_mqtt_try_capabilities(') + '\n' + function(mqtt,'int service_mqtt_snapshot(')
    fixture = (ROOT/'tests/http_core/status_budget.c').read_text().replace('/* PRODUCTION_MQTT_WIRE */',mqtt_fixture)
    if dual:
        fixture = '#define PHICOMM_BEMFA 1\n'+fixture
        snapshots += '\nvoid bemfa_status_capture(struct device_network *s){s->mqtt_enabled=true;s->bemfa_enabled=true;s->bemfa_status=5;s->bemfa_last_stage=5;}\n'
    return fixture.replace('/* PRODUCTION_WATCHDOG_FILTER */', snapshots+watchdog+'\n'+filter_fn).replace('/* PRODUCTION_BUILDERS */', builders).replace('/* PRODUCTION_RESULT_QUERY */', result+'\n'+query)


def background_fixture():
    runtime=(ROOT/'common/runtime.c').read_text()
    engine=(ROOT/'common/engine.c').read_text()
    functions='\n'.join(function(engine,s) for s in (
        'bool feature_sample_fresh(', 'unsigned feature_aggregate(',
        'bool feature_co2_fresh(', 'unsigned feature_co2_aggregate('))
    functions+='\n#define number integer\n'+'\n'.join(function(runtime,s) for s in (
        'static void co2_body(', 'static json_object *sample_body(',
        'static json_object *average_body('))+'\n#undef number\n'
    fixture=status_fixture();fixture=fixture[:fixture.index('int main(void){')]
    return fixture+'\n#include <math.h>\n'+functions+r'''
static char background_golden[2048];
static bool nested_ok,owned_background;
static int nested_status(const char *topic,const char *body,bool retain){
 assert(!strcmp(topic,"test/average")&&!retain&&!strcmp(body,background_golden));
 json_heap_stats_t stats;json_heap_snapshot(&stats);
 assert(owned_background?stats.live_blocks==2:stats.live_blocks>2);
 json_object *reply=service_query("status.get");
 struct printbuf *wire=json_owned_to_wire(reply);
 nested_ok=wire!=NULL;if(wire)printbuf_free(wire);
 assert(!strcmp(body,background_golden));return -7;
}
static json_object *background_graph(void){
 feature_sample_t samples[3]={{.valid=true,.received_ms=100,.value={22,45,100,.05},
  .co2_supported=true,.co2_valid=true,.co2=1100,.co2_received_ms=100}};
 return average_body(samples,3,100);
}
int main(void){
 char warm[64];snprintf(warm,sizeof(warm),"%f",1.25);libc_baseline=live;healthy_stats();
 json_object *golden=background_graph();const char *text=json_object_to_json_string(golden);
 assert(text&&strlen(text)<sizeof(background_golden));strcpy(background_golden,text);
 json_object_put(golden);healthy_stats();
 for(unsigned owned=0;owned<2;owned++){
  setup(0);capacity=25000+libc_baseline;peak=fail_size=fail_phase=0;owned_background=owned;
  json_object *o=background_graph();assert(o);
  if(owned)json_owned_publish(nested_status,"test/average",o,false);
  else {const char *wire=json_object_to_json_string(o);assert(wire);nested_status("test/average",wire,false);json_object_put(o);}
  assert(nested_ok==owned);healthy_stats();
  char report[160];int n=snprintf(report,sizeof(report),"background %s actual-average+nested-full capacity=25000 peak=%u guard=%u full=%u live=0\n",owned?"owned":"legacy",(unsigned)(peak-libc_baseline),(unsigned)fail_size,nested_ok);_write(1,report,n);
 }
 return 0;
}
'''


def virtual_fixture(retain_config=False):
    """Unchanged complete periodic producer, actual config sizeof + allocator ABI."""
    runtime=(ROOT/'common/runtime.c').read_text()
    fixture=background_fixture().split('static char background_golden')[0]
    # Real production token functions; only the RTOS mutex boundary is inert.
    service=(ROOT/'common/service.c').read_text()
    fixture=fixture.replace('unsigned service_json_claim(void){return 1;}\nvoid service_json_release(unsigned token){(void)token;}',r'''
static bool ready=true;static mico_mutex_t service_lock;
static unsigned json_owner,json_generation;
'''+function(service,'unsigned service_json_claim(')+'\n'+function(service,'void service_json_release(').replace('if (json_scope.token==token) memset(&json_scope,0,sizeof(json_scope));','if(fixture_scope_token==token)fixture_scope_token=fixture_scope_failed=0;'))
    fixture=fixture.replace('uint32_t mico_rtos_get_time(void){return 25000;}','static uint32_t now=25000;\nuint32_t mico_rtos_get_time(void){return now;}')
    fixtures=r'''
#define FEATURE_DEVICE_A1
#define a1_state state
#define LOCAL_ID a1_id
#define COUNT(x) (sizeof(x)/sizeof(*(x)))
static feature_config_t configuration;
/* DISCOVERY_LAYOUT */
static discovery_layout_t *published_layout;
static uint32_t mqtt_discovery_retry_at;static bool mqtt_discovery_deferred,mqtt_capture_deferred;
static feature_sample_t samples[FEATURE_MAX_PEERS];
static uint32_t mqtt_tick,mqtt_graph_retry,mqtt_revision;
static unsigned mqtt_graph_pending,mqtt_leaf_pending,published_count,published_sensors;
static uint8_t mqtt_telemetry_cursor;
static unsigned mqtt_discovery_stage,mqtt_discovery_cursor,mqtt_domain_cursor,mqtt_discovery_sensors;
static uint32_t mqtt_discovery_revision;static bool mqtt_discovery_pending;
static int publish_error;
static bool mqtt_online=true,was_coordinator=true;
static char published_root[FEATURE_TOPIC_MAX],published_ids[FEATURE_MAX_PEERS][13];
static struct {bool light_sleep;unsigned brightness;} sleep_state;

static const char *domains[]={"unused"};
void feature_config_get(feature_config_t*c){*c=configuration;}
bool feature_bound(void){return true;}
bool feature_is_coordinator(void){return true;}
static unsigned discovery_sensors(const feature_config_t*c){(void)c;return 0;}
static void root_for(const feature_config_t*c,unsigned i,char*out){(void)c;(void)i;strcpy(out,"test");}
static bool discovery_virtual_step(int(*p)(const char*,const char*,bool),const discovery_layout_t*c,const char*r,bool b,unsigned*cursor){(void)p;(void)c;(void)r;(void)b;(void)cursor;abort();}
static json_object *config_encode(const feature_config_t*c,const char*d){(void)c;(void)d;abort();}
static const char *topic_domain(const char*d){return d;}
static void js(json_object*o,const char*k,const char*v){json_build_object_add(o,k,json_object_new_string(v));}
static void lock(void){}static void unlock(void){}
static uint32_t now_ms(void){return now;}
void feature_colon_mac(const char*id,char*out){(void)id;strcpy(out,"11:22:33:44:55:66");}
static bool mqtt_format(char*out,size_t n,const char*format,...){va_list args;va_start(args,format);int used=vsnprintf(out,n,format,args);va_end(args);return used>=0&&(size_t)used<n;}
'''
    fixtures=fixtures.replace('static bool mqtt_format(', 'static bool fixture_format(').replace('static bool fixture_format(', 'static bool fixture_format(')
    # mqtt_account.h already implements mqtt_format, so use it unchanged.
    fixtures=fixtures[:fixtures.index('static bool fixture_format(')]
    fixtures=fixtures.replace('/* DISCOVERY_LAYOUT */',runtime[runtime.index('typedef struct {\n  char group[33];'):runtime.index('static discovery_layout_t *published_layout;')])
    mqtt=(ROOT/'common/mqtt/service.c').read_text()
    fixtures += '\n#include \"common/mqtt/service.h\"\n#define PH_MQTT_ERROR_APP_DISCOVERY_GRAPH 8\n#define PH_MQTT_ERROR_APP_DISCOVERY_ENCODING 10\nstatic int ph_mqtt_app_error(unsigned stage,int rc){(void)stage;return rc;}\n'+ '\n'.join(function(mqtt,k) for k in ('bool service_mqtt_json_begin(', 'static unsigned service_mqtt_json_finish(', 'void service_mqtt_json_end(', 'int service_mqtt_json_error(', 'int service_mqtt_optional_send('))
    fixtures += '\n'+function(runtime,'static void discovery_layout_capture(')+'\n'+function(runtime,'static void discovery_resource_wait(')
    functions='\n'.join(function(runtime,s) for s in ('static int direct_leaf(', 'static __attribute__((noinline)) int direct_json_optional(', 'static void virtual_service(int (*publish)(const char *, const char *, bool)) {'))
    if retain_config:
        functions=functions.replace('free(c);c=NULL;service_json_release(snapshot_token);','service_json_release(snapshot_token);')
    return fixture+fixtures+'\n'+('#define RETAIN_CONFIG\n' if retain_config else '')+functions+r'''
static json_object *background_graph(void){
 feature_sample_t current[1]={{.valid=true,.received_ms=25000,.value={22,45,100,.05}}};
 return average_body(current,1,25000);
}
static bool expect_full;static unsigned callbacks,full_ok;static size_t sender_live;
static int nested_sender(const char*topic,const char*body,bool retain){
 (void)retain;assert(topic&&body);callbacks++;
 /* The native config is no longer retained at ANY periodic sender, including
  * scalar leaves before average JSON. Wire bytes can still remain. */
 unsigned token=service_json_claim();assert(token);
 sender_live=live-libc_baseline;
#ifdef RETAIN_CONFIG
 assert(sender_live>=sizeof(feature_config_t));
#else
 assert(sender_live<sizeof(feature_config_t));
#endif
 json_object *response=service_query("status.get");struct printbuf *wire=json_owned_to_wire(response);
 assert((wire!=NULL)==expect_full);if(wire){full_ok++;printbuf_free(wire);}
 service_json_release(token);return 0;
}
int main(void){
 char warm[64];snprintf(warm,sizeof(warm),"%f",1.25);libc_baseline=live;healthy_stats();
 /* Actual graph allocations are retained while another HTTP operation asks
  * for admission. Exclusion returns WAIT without a new graph or denial. */
 unsigned graph_token=service_json_claim();assert(graph_token);json_object *graph=background_graph();assert(graph);
 ph_http_product waiting_product={0};ph_http_exchange waiting_exchange={0};
 uint64_t failures=service_memory_failures();
 assert(ph_http_claim(&waiting_product,&waiting_exchange,0,false)==PH_HTTP_WAIT);
 assert(service_memory_failures()==failures&&json_owner==graph_token);
 ph_http_product_release(&waiting_product);json_object_put(graph);service_json_release(graph_token);healthy_stats();
 configuration.peer_count=1;configuration.revision=mqtt_revision=1;strcpy(configuration.group,"room");
 strcpy(configuration.peers[0].id,"112233445566");
 samples[0]=(feature_sample_t){.valid=true,.received_ms=25000,.value={22,45,100,.05},
  .co2_supported=true,.co2_valid=true,.co2=1100,.co2_received_ms=25000};
 for(unsigned trial=0;trial<2;trial++){
  size_t budget=trial?25800:24000;setup(0);capacity=budget+libc_baseline;peak=fail_size=0;callbacks=full_ok=0;
  mqtt_tick=0;now=25000;expect_full=trial;
#ifdef RETAIN_CONFIG
  expect_full=false;
#endif
  for(unsigned turn=0;turn<16;turn++){
    virtual_service(nested_sender);
    if(!mqtt_graph_pending&&!mqtt_leaf_pending)break;
    now+=100;
  }
  assert(!mqtt_leaf_pending);
  assert(callbacks>=7 && full_ok==(expect_full?callbacks:0));assert(!json_owner&&!mqtt_graph_pending);healthy_stats();
  char report[256];int n=snprintf(report,sizeof(report),"actual virtual_service retained=%u config=%u budget=%u callbacks=%u full=%u peak=%u denied=%u last_sender_wire=%u live=0\n",
#ifdef RETAIN_CONFIG
  1u,
#else
  0u,
#endif
  (unsigned)sizeof(feature_config_t),(unsigned)budget,callbacks,full_ok,(unsigned)(peak-libc_baseline),(unsigned)fail_size,(unsigned)sender_live);_write(1,report,n);
 }
 /* Native config allocation failure releases its snapshot token as well. */
 native_fail_size=sizeof(feature_config_t);now+=6000;unsigned before=callbacks;
 virtual_service(nested_sender);assert(callbacks==before&&!json_owner);healthy_stats();
 native_fail_size=0;now+=1000;virtual_service(nested_sender);assert(callbacks>before&&!json_owner);healthy_stats();
 /* Busy snapshot admission does no native allocation/marking, then recovers. */
 unsigned token=service_json_claim();assert(token);now+=6000;unsigned sent=callbacks;
 virtual_service(nested_sender);assert(callbacks==sent);service_json_release(token);
 virtual_service(nested_sender);assert(callbacks>sent&&!json_owner);healthy_stats();
 return 0;
}
'''

class JsonBudget(unittest.TestCase):
    def test_arm32_actual_virtual_sender(self):
        for retained in (True,False):
            with self.subTest(retained=retained):
                self.compile_arm("A1", virtual_fixture(retained), budget=True)

    def test_arm32_background_overlap(self):
        self.compile_arm('A1', background_fixture(), budget=True)

    def test_arm32_actual_status_budget(self):
        self.compile_arm('A1', status_fixture(), budget=True)

    def test_arm32_dual_mqtt_status_budget(self):
        fixture=status_fixture(dual=True)
        fixture=fixture[:fixture.index('int main(void){')]+'''int main(void){
 char warm[64];snprintf(warm,sizeof(warm),"%f",1.25);libc_baseline=live;healthy_stats();
 measure(true,0,false,25000,true);
 measure(true,0,false,23000,false);
 measure(true,0,false,25000,true);
 return 0;
}
'''
        self.compile_arm('A1',fixture,budget=True)

    def test_owned_json_all_sdk_arm(self):
        for model in ('A1','M1','TC1'):
            with self.subTest(model=model):
                self.compile_arm(model, (ROOT/'tests/http_core/json_owned.c').read_text(), budget=False)

    def compile_arm(self, model, fixture, budget, product=True, host=False):
        sdk = Path(os.environ.get('PHICOMM_'+model+'_SDK', ROOT/'build'/('tc1-sdk-3.6.4' if model=='TC1' else model.lower()+'-sdk')/'mico-os'))
        cc = Path(os.environ.get('ARM_CC', ROOT/'build/tc1-tools/gcc-arm-none-eabi-5_4-2016q2/bin/arm-none-eabi-gcc'))
        qemu = shutil.which('qemu-arm') or shutil.which('qemu-arm-static')
        if not cc.exists() or not (sdk/'include/mico.h').exists() or not qemu:
            self.skipTest(model+' real SDK, pinned compiler and QEMU required')
        tc1 = model=='TC1'
        mcu, board = ('MW3xx','MK3031') if tc1 else ('MX1290','MK3080B')
        includes = ['include','platform','platform/include','platform/Cortex-M4',
            'platform/Cortex-M4/CMSIS','platform/MCU/'+mcu,'platform/MCU/'+mcu+'/peripherals',
            'platform/MCU/'+mcu+'/interface','board/'+board,'MiCO/system','MiCO/system/include',
            'libraries/utilities','MiCO/security','MiCO/security/TLS/mocSSL','libraries/utilities/json_c']
        if tc1:
            includes += ['libraries/drivers','libraries/drivers/MiCOKit_EXT']
            includes += ['platform/MCU/MW3xx/peripherals/sdk/src/incl/'+p for p in
                ('sdk','platform/os/micortos','sdk/drivers','sdk/drivers/mw300','sdk/drivers/mw300/regs')]
        flags = ['-std=gnu11','-g','-Os','-mthumb','-mcpu=cortex-m3' if tc1 else '-mcpu=cortex-m4',
            '-DMOC=1','-DMW310' if tc1 else '-DCONFIG_CPU_MX1290','-I'+str(ROOT),'-I'+str(ROOT/model),'-I'+str(ROOT/'common')]
        if tc1:
            flags += ['-DCONFIG_VARIANT_MW310']
        else:
            flags += ['-mfpu=fpv4-sp-d16','-mfloat-abi=hard']
        flags += ['-I'+str(sdk/p) for p in includes]
        host_flags = [f for f in flags if f.startswith(('-I','-D','-std='))]
        flags += ['-u','_printf_float','-u','_scanf_float','-nostartfiles','--specs=nano.specs',
            '--specs=nosys.specs','-Wl,-e,_start,-Ttext=0x10000']
        if budget:
            flags += ['-Wl,--wrap='+s for s in ('malloc','calloc','realloc','free','strdup','_malloc_r','_calloc_r','_realloc_r','_free_r')]
        with tempfile.TemporaryDirectory() as tmp:
            json=copy_patched_json(sdk/'libraries/utilities/json_c',Path(tmp)/'sdk')
            # SDK headers use json_c/json.h; encoders also use json.h. Both
            # must resolve to the same patched copy, including linkhash layout.
            flags[0:0] = ['-I'+str(json), '-I'+str(json.parent)]
            retained = os.environ.get('PHICOMM_HTTP_JSON_ARTIFACT_DIR')
            directory = Path(retained).resolve()/('budget' if budget else model) if retained else Path(tmp)
            directory.mkdir(parents=True,exist_ok=True)
            source = directory/'probe.c'
            if product:
                fixture+='\n#include "tests/http_core/wire_stubs.inc"\n'
            source.write_text(fixture)
            binary = directory/'probe.elf'
            obj=directory/'json_owned.o'
            subprocess.run([str(cc),*flags,'-fstack-usage','-c',str(ROOT/'common/json_owned.c'),'-o',str(obj)],check=True,timeout=30)
            json_objects=[]
            # Embedded JSON uses the checked private numeric implementation;
            # the host shim below deliberately needs only the top-level files.
            for path in [*json.glob('*.c'), *(json/'json_numeric').glob('*.c')]:
                target=directory/(path.stem+'.o');json_objects.append(str(target))
                subprocess.run([str(cc),*flags,'-fstack-usage','-c',str(path),'-o',str(target)],check=True,timeout=30)
            subprocess.run([str(cc),*flags,str(source),str(ROOT/'tests/http_core/json_arm_runtime.c'),
                *([str(ROOT/'common/httpd/product.c')] if product else []),str(obj),*json_objects,
                '-o',str(binary)],check=True,timeout=30)
            subprocess.run([qemu,str(binary)],check=True,timeout=30)
            if host:
                # Real SDK declarations/layout on ASan host, with only the
                # SDK's 32-bit-long format assumption adapted for host libc.
                for path in json.glob('*.c'):
                    path.write_text(path.read_text().replace('%ld','%d'))
                host_binary=directory/'probe-host'
                sanitize=['-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
                host_cc=os.environ.get('CC','cc');host_obj=directory/'probe-host.o'
                subprocess.run([host_cc,'-I'+str(json),'-I'+str(json.parent),*host_flags,
                    '-Dsize_t=size_t','-Dssize_t=ssize_t',*sanitize,
                    '-c',str(source),'-o',str(host_obj)],check=True,timeout=30)
                # JSON host compilation only needs the established StringUtils
                # stub; product capture above still uses actual SDK headers.
                subprocess.run([host_cc,'-std=gnu11','-I'+str(ROOT/'tests/stubs'),
                    '-I'+str(json),*sanitize,str(host_obj),
                    *[str(p) for p in json.glob('*.c')],'-o',str(host_binary)],check=True,timeout=30)
                subprocess.run([str(host_binary)],check=True,timeout=30)



if __name__=='__main__':
    unittest.main()

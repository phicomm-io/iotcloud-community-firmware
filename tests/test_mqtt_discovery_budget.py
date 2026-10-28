"""Actual virtual_service lifecycle with bounded HA and native MP output.

Transport and native snapshots are fakes. Production discovery cursor, config
capture/release, retry/backoff and bounded HA writer run unchanged on ARM32.
The align8+8 allocation ledger is a sensitivity model, not a MiCO heap emulator.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from tests.test_runtime_mqtt_mp import function
ROOT = Path(__file__).resolve().parents[1]


def discovery_fixture():
    runtime = (ROOT/'common/runtime.c').read_text()
    fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FEATURE_HOST 1
#include "common/phicomm_features.h"
#include "common/typed_wire.h"
#include "common/ha_json.h"
#include "common/device_status.h"
#define FEATURE_DEVICE_A1 1
#define LOCAL_ID "aabbccddeeff"
#define PH_MQTT_MORE 1
#define PH_MQTT_ERROR_APP_DISCOVERY_GRAPH -20
#define PH_LANG_ZH 0
static uint32_t now=UINT32_MAX-500;
uint32_t mico_rtos_get_time(void){return now;}
static uint32_t now_ms(void){return now;}
static unsigned device_language_get(void){return PH_LANG_ZH;}
static const char *ph_i18n(unsigned l,const char*z,const char*e){return l?e:z;}
typedef struct {char name[64];unsigned brightness,red,green,blue,speed;bool fan_on,child_lock,led_always_on;}a1_config_t;
static feature_config_t configuration;
static mqtt_account_t account;
static bool coordinator=true;
static unsigned captures;
void feature_config_get(feature_config_t*c){captures++;*c=configuration;}
bool feature_bound(void){return true;}
bool feature_is_coordinator(void){return coordinator;}
static mqtt_account_t mqtt_snapshot(void){return account;}
static void root_for(const feature_config_t*c,unsigned i,char*out){(void)c;(void)i;strcpy(out,"phicomm/v2/review/group/G");}
static void a1_config_get(a1_config_t*c){memset(c,0,sizeof(*c));memset(c->name,1,63);}
void feature_colon_mac(const char*x,char*out){(void)x;strcpy(out,"aa:bb:cc:dd:ee:ff");}
void feature_effective_fan(bool on,unsigned speed,bool*a,unsigned*b){*a=on;*b=speed;}
bool feature_fan_auto(void){return false;}
static unsigned a1_lock;
static void mico_rtos_lock_mutex(unsigned*x){assert(!*x);*x=1;}
static void mico_rtos_unlock_mutex(unsigned*x){assert(*x);*x=0;}
static struct {bool led_visible;}a1_state;
static feature_sleep_state_t sleep_state;
static void lock(void){}
static void unlock(void){}
static const char *domains[]={"binding","linkage","light","schedule","sleep","time"};
static const char *topic_domain(const char*d){return d;}
/* Allocation ledger counts native captures, retained compact layout, and every
 * binary buffer growth including copy-before-free; HA uses fixed scratch. */
static size_t live,peak,capacity=100000;
static unsigned denies,native_attempts;
static size_t fail_native_size;
static void *native_config_live;
static size_t charge(size_t n){return ((n+7)&~(size_t)7)+8;}
static void *tracked_malloc(size_t n){
 native_attempts++;size_t cost=charge(n);
 if(n==fail_native_size || cost>capacity-live){denies++;return NULL;}
 size_t*h=malloc(cost);assert(h);*h=cost;live+=cost;if(live>peak)peak=live;
 void*p=(unsigned char*)h+8;if(n==sizeof(feature_config_t)){assert(!native_config_live);native_config_live=p;}return p;
}
static void tracked_free(void*p){if(!p)return;if(p==native_config_live)native_config_live=NULL;size_t*h=(size_t*)((unsigned char*)p-8);assert(*h<=live);live-=*h;free(h);}
void *ph_wire_malloc(size_t n){return tracked_malloc(n);}
void ph_wire_free(void*p){tracked_free(p);}
static bool domain_fail;
static ph_buf *domain_config_wire_mp(const feature_config_t*c,const char*d,bool source){
 assert(c==native_config_live&&source);if(domain_fail)return NULL;
 ph_writer w;if(!phw_init(&w,1))return NULL;phw_begin(&w,NULL,false);phw_text(&w,"domain",d);phw_u32(&w,"revision",c->revision);phw_end(&w);return phw_take(&w);
}
static ph_buf *status(void){
 device_status_t s={0};device_status_full_t f={0};s.model=2;s.captured=63;s.identity.version="1.0.0-rc.4";strcpy(s.identity.id,LOCAL_ID);strcpy(s.identity.name,"overlap");f.watchdog.fault="";f.watchdog.reset_name="unknown";
 ph_writer w;if(!phw_init(&w,1))return NULL;device_status_mp(&w,&s,&f);return phw_take(&w);
}
static int publish_error;
static uint32_t mqtt_revision,mqtt_tick;
static bool mqtt_online;
static unsigned mqtt_leaf_pending;
static uint8_t mqtt_telemetry_cursor;
/* PRODUCTION_GLOBALS */
static unsigned sends,ha_sends,binary_sends,overlaps;static size_t max_ha;static bool nested,transport_fail;
static int sender(const char*t,const void*b,size_t n,bool retain){
 assert(t&&b&&retain&&!native_config_live);sends++;
 if(strstr(t,"homeassistant/")){ha_sends++;if(n){assert(((const char*)b)[0]=='{'&&((const char*)b)[n-1]=='}');assert(n<5120);if(n>max_ha)max_ha=n;}}
 else if(strstr(t,"/state")&&n&&((const unsigned char*)b)[0]!= 'O'){
  ph_value v;if(phv_parse(1,b,n,&v)){binary_sends++;assert(v.kind==PHV_MAP);}
 }
 if(nested){size_t before=live;ph_buf *wire=status();assert(wire);ph_value v;assert(phv_parse(1,wire->buf,wire->len,&v));ph_buf_free(wire);assert(live==before);overlaps++;}
 return transport_fail?-17:0;
}
static int direct_leaf(int(*p)(const char*,const void*,size_t,bool),const char*root,const char*leaf,const char*b,bool retain){char topic[512];assert(mqtt_format(topic,sizeof(topic),"%s/%s",root,leaf));return p(topic,b,strlen(b),retain);}
#define malloc tracked_malloc
#define free tracked_free
/* PRODUCTION */
#undef malloc
#undef free
static void configure(void){
 assert(!native_config_live);if(published_layout){tracked_free(published_layout);published_layout=NULL;}assert(!live);
 memset(&configuration,0,sizeof(configuration));configuration.revision=1;memset(configuration.group,'G',32);
 configuration.peer_count=4;configuration.member_count=8;configuration.event_count=8;configuration.sleep_count=4;
 for(unsigned i=0;i<4;i++)snprintf(configuration.peers[i].id,13,"%012x",i+16);
 for(unsigned i=0;i<8;i++){snprintf(configuration.members[i].id,13,"%012x",i+1);strcpy(configuration.members[i].ip,"192.168.111.111");}
 mqtt_online=false;was_coordinator=true;mqtt_discovery_pending=mqtt_discovery_deferred=mqtt_capture_deferred=false;
 mqtt_revision=0;mqtt_tick=now;mqtt_leaf_pending=0;published_root[0]=0;published_count=0;publish_error=0;
}
int main(void){
#ifdef ARM_TEST
 assert(sizeof(void*)==4&&sizeof(long)==4&&sizeof(feature_config_t)==1408);
#endif
 assert(sizeof(discovery_layout_t)<=320);assert(mqtt_account_init(&account,1,"a1","review",LOCAL_ID));configure();
 /* True config-allocation OOM: one retry each second, even across uint32 wrap. */
 fail_native_size=sizeof(feature_config_t);unsigned before=native_attempts;
 virtual_service(sender);assert(mqtt_capture_deferred&&mqtt_discovery_deferred&&!live&&!captures);
 for(unsigned i=0;i<49;i++){now+=20;virtual_service(sender);assert(native_attempts==before+1);}
 now+=20;virtual_service(sender);assert(native_attempts==before+2&&now<1000&&!live);
 fail_native_size=0;now+=1000;virtual_service(sender);assert(mqtt_discovery_stage==1&&!native_config_live&&!live);
 /* Full configuration must be freed before fixed HA discovery publishes. */
 nested=true;for(unsigned i=0;i<100&&!mqtt_online;i++){publish_error=0;virtual_service(sender);now+=20;assert(!native_config_live);}
 assert(mqtt_online&&!mqtt_discovery_pending&&published_layout&&published_layout->peer_count==4&&published_layout->member_count==8);
 assert(ha_sends==39&&binary_sends==6&&overlaps==sends&&max_ha<5120);size_t quiet=live;assert(quiet==charge(sizeof(discovery_layout_t)));
 /* Native snapshots also recover from low-budget overlap without changing the
  * retained compact history. This does not claim real-device heap capacity. */
 capacity=quiet;ph_buf *wire=status();assert(!wire&&live==quiet);capacity=100000;wire=status();assert(wire);ph_buf_free(wire);assert(live==quiet);
 /* Domain MP output refusal keeps its cursor and backs off, then recovers. */
 configure();virtual_service(sender);while(mqtt_discovery_stage<2){virtual_service(sender);now+=20;}
 domain_fail=true;virtual_service(sender);assert(mqtt_discovery_deferred&&mqtt_domain_cursor==0&&!native_config_live);
 unsigned captured=captures;now+=999;virtual_service(sender);assert(captures==captured+1&&mqtt_domain_cursor==0);
 domain_fail=false;now++;virtual_service(sender);assert(mqtt_domain_cursor==1&&!mqtt_discovery_deferred);
 /* History OOM at completion retries only after backoff, without re-emitting
  * successful discovery/domain frames. */
 while(mqtt_domain_cursor<6){virtual_service(sender);now+=20;}
 fail_native_size=sizeof(discovery_layout_t);unsigned sent=sends;virtual_service(sender);assert(mqtt_discovery_pending&&mqtt_discovery_deferred&&!published_layout&&!native_config_live);
 now+=999;virtual_service(sender);assert(sends==sent&&!published_layout);
 fail_native_size=0;now++;virtual_service(sender);assert(mqtt_online&&published_layout&&sends==sent);
 /* Real transport failure preserves the discovery cursor for its next turn. */
 configure();virtual_service(sender);transport_fail=true;virtual_service(sender);assert(publish_error==-17&&mqtt_discovery_cursor==0&&!native_config_live);
 transport_fail=false;publish_error=0;virtual_service(sender);assert(mqtt_discovery_cursor==1);
 configure();assert(!live&&denies>=4&&overlaps);
 char report[256];int n=snprintf(report,sizeof(report),"virtual_service native=%u compact=%u HA_max=%u overlap_peak=%u denied=%u live=0 PASS\n",(unsigned)sizeof(feature_config_t),(unsigned)sizeof(discovery_layout_t),(unsigned)max_ha,(unsigned)peak,denies);
#ifdef ARM_TEST
 extern int _write(int,const void*,int);_write(1,report,n);
#else
 fwrite(report,1,n,stdout);
#endif
 return 0;
}
'''
    globals_start = runtime.index('static unsigned mqtt_discovery_stage')
    globals_end = runtime.index('static unsigned published_count;', globals_start)+len('static unsigned published_count;')
    fixture = fixture.replace('/* PRODUCTION_GLOBALS */', runtime[globals_start:globals_end])
    return fixture.replace('/* PRODUCTION */', '\n'.join(function(runtime, s) for s in (
        'static bool virtual_identity(', 'static const char *virtual_device_name(',
        'static bool discovery_virtual_step(', 'static int direct_wire_mp(',
        'static void virtual_service(int (*publish)(const char *, const void *, size_t, bool)) {')))


class DiscoveryBudget(unittest.TestCase):
    def compile_run(self, arm, fixture=None, negative=False):
        qemu = shutil.which('qemu-arm') or shutil.which('qemu-arm-static')
        cc = Path(os.environ.get('ARM_CC', ROOT/'build/a1-tools/gcc-arm-none-eabi-5_4-2016q2/bin/arm-none-eabi-gcc')) if arm else shutil.which('cc')
        self.assertTrue(cc and (not arm or cc.is_file()), 'prepare pinned compiler or set ARM_CC')
        if arm:self.assertTrue(qemu, 'ARM32 regression requires QEMU')
        with tempfile.TemporaryDirectory() as tmp:
            src, binary = Path(tmp)/'fixture.c',Path(tmp)/'fixture'
            src.write_text(fixture or discovery_fixture())
            flags=['-std=gnu99','-Os','-I'+str(ROOT),'-I'+str(ROOT/'common'),'-I'+str(ROOT/'tests/host')]
            if arm:flags+=['-DARM_TEST','-mthumb','-mcpu=cortex-m3','-nostartfiles','--specs=nano.specs','--specs=nosys.specs','-Wl,-e,_start,-Ttext=0x10000']
            else:flags+=['-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
            production=['common/device_status.c','common/typed_wire.c','common/wire_buffer.c']
            if arm:production+=['tests/http_core/json_arm_runtime.c']
            result=subprocess.run([str(cc),*flags,str(src),*[str(ROOT/p) for p in production],'-lm','-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run(([qemu] if arm else [])+[str(binary)],capture_output=True,text=True,timeout=30)
            if negative:
                self.assertNotEqual(result.returncode,0,"mutated retry/release must fail the lifecycle assertions")
            else:
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                print(result.stdout.strip())

    def test_actual_arm_discovery_retry_exclusion_and_recovery(self):
        self.compile_run(True)

    def test_host_discovery_lifecycle_sanitizers(self):
        self.compile_run(False)

    def test_missing_capture_backoff_negative_control(self):
        fixture=discovery_fixture()
        original="if(mqtt_capture_deferred && (uint32_t)(now-mqtt_discovery_retry_at)<1000u)return;"
        self.assertEqual(fixture.count(original),1)
        self.compile_run(False,fixture.replace(original,"/* deliberately remove capture backoff */"),negative=True)

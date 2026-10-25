"""Actual ARM virtual discovery producer: lease, bounded failure and recovery.

Only RTOS/native captures and transport are simulated. The production JSON
codec, discovery builder, resumable producer and pinned SDK allocator run on
ARM32. align8+8 is an allocator sensitivity model, not a MiCO heap emulator.
"""
from pathlib import Path
import unittest
from tests.test_http_json_budget import ROOT, function, virtual_fixture
from tests import test_http_json_budget as budget


def discovery_fixture():
    runtime=(ROOT/'common/runtime.c').read_text()
    fixture=virtual_fixture(False)
    fixture=fixture[:fixture.index('static json_object *background_graph(void){',fixture.index('static void virtual_service(int (*publish)'))]
    stub=function(fixture,'static bool discovery_virtual_step(')
    production='static void js(json_object*,const char*,const char*);\n#define number integer\n'
    production+='static mqtt_account_t mqtt_snapshot(void){mqtt_account_t a;assert(mqtt_account_init(&a,1,"a1","review","aabbccddeeff"));return a;}\n'
    production+='\n'.join(function(runtime,s) for s in ('static bool virtual_identity(', 'static const char *virtual_device_name(', 'static bool discovery_virtual_step('))
    production+='\n#undef number\n'
    # Optional graph helpers and retry state must precede the real builder.
    fixture=fixture.replace(stub,'static bool discovery_virtual_step(int(*)(const char*,const char*,bool),const discovery_layout_t*,const char*,bool,unsigned*);')
    start=fixture.index('static void virtual_service(int (*publish)')
    fixture=fixture[:start]+production+'\n'+fixture[start:]
    codec=(ROOT/'common/config_json.c').read_text()
    codec=codec[:codec.index('static bool config_decode(')].replace('#define COUNT(a) (sizeof(a) / sizeof((a)[0]))','').replace('#include "phicomm_features.h"','#include "common/phicomm_features.h"')
    fixture=fixture.replace(function(fixture,'static json_object *config_encode('),codec)
    fixture=fixture.replace('static const char *domains[]={"unused"};','static const char *domains[]={"binding","linkage","light","schedule","sleep","time"};')
    # js from the production codec replaces the fixture's identical seam.
    needle='static void js(json_object*o,const char*k,const char*v){json_build_object_add(o,k,json_object_new_string(v));}'
    fixture=fixture.replace(needle,'')
    fixture=fixture.replace('static unsigned fixture_scope_token,fixture_scope_failed;', 'static unsigned fixture_scope_token,fixture_scope_failed,scope_attempts;\n')
    fixture=fixture.replace('fixture_scope_token=token;fixture_scope_failed=0;return true;','fixture_scope_token=token;fixture_scope_failed=0;scope_attempts++;return true;')
    fixture=fixture.replace('void feature_config_get(feature_config_t*c){*c=configuration;}','void feature_config_get(feature_config_t*c){assert(!native_config_live);native_config_live=c;*c=configuration;}')
    fixture=fixture.replace('static struct {void *p;size_t n;} ledger[2048];','static void *native_config_live;static bool require_lease;static unsigned json_owner,fault_at,fault_calls;\nstatic struct {void *p;size_t n;} ledger[2048];')
    fixture=fixture.replace('int json_heap_permit(size_t n){int ok=', 'int json_heap_permit(size_t n){if(require_lease)assert(json_owner);if(fault_at && ++fault_calls==fault_at)return 0;int ok=')
    fixture=fixture.replace('void __wrap_free(void*p){bool outer=', 'void __wrap_free(void*p){if(p==native_config_live)native_config_live=NULL;bool outer=')
    fixture = '#include "common/i18n.h"\nuint8_t device_language_get(void){return PH_LANG_ZH;}\n' + fixture
    return fixture+r'''
static unsigned sends,quota_count;static bool enforce_nested,quota,transport_fail;
static int sender(const char*t,const char*b,bool retain){
 assert(t&&b&&retain);assert(!json_owner&&!fixture_scope_token&&!native_config_live);sends++;
 json_heap_stats_t j;json_heap_snapshot(&j);assert(j.live_blocks==0 || j.live_blocks==2);
 if(enforce_nested){
  require_lease=false;json_object *r=service_query("status.get");struct printbuf *wire=json_owned_to_wire(r);assert(wire);printbuf_free(wire);require_lease=true;
 }
 if(transport_fail)return -17;
 if(quota && ++quota_count>1){publish_error=PH_MQTT_MORE;return PH_MQTT_MORE;}
 return 0;
}
static void configure(void){
 memset(&configuration,0,sizeof(configuration));configuration.revision=1;
 memset(configuration.group,'G',sizeof(configuration.group)-1);
 configuration.peer_count=FEATURE_MAX_PEERS;configuration.member_count=FEATURE_STORAGE_PEERS;
 configuration.event_count=FEATURE_MAX_EVENTS;configuration.sleep_count=FEATURE_MAX_SLEEP;
 for(unsigned i=0;i<FEATURE_MAX_PEERS;i++){
  snprintf(configuration.peers[i].id,13,"%012x",i+16);
  memset(configuration.peers[i].group,'P',sizeof(configuration.peers[i].group)-1);
  strcpy(configuration.peers[i].ip,"192.168.111.111");memset(configuration.peers[i].token,0xab,16);
 }
 for(unsigned i=0;i<FEATURE_STORAGE_PEERS;i++){
  snprintf(configuration.members[i].id,13,"%012x",i+1);strcpy(configuration.members[i].ip,"192.168.111.111");
 }
 memset(configuration.ntp_host,'N',sizeof(configuration.ntp_host)-1);
 mqtt_online=false;was_coordinator=true;mqtt_discovery_pending=false;mqtt_discovery_deferred=false;
 mqtt_revision=0;mqtt_tick=now;mqtt_graph_pending=mqtt_leaf_pending=0;published_root[0]=0;
}
int main(void){
 char warm[64];snprintf(warm,sizeof(warm),"%f",1.25);libc_baseline=live;healthy_stats();setup(1);
 assert(sizeof(feature_config_t)==1408 && sizeof(discovery_layout_t)<=320);
 configure();require_lease=true;capacity=libc_baseline+22000;now=UINT32_MAX-500;mqtt_tick=now;
 json_heap_stats_t before,after;json_heap_snapshot(&before);unsigned attempts=scope_attempts;
 /* Begin stage zero, then a genuine allocation failure. Backoff crosses wrap. */
 virtual_service(sender);assert(mqtt_discovery_stage==1);
 virtual_service(sender);assert(mqtt_discovery_deferred&&!mqtt_discovery_cursor);
 for(unsigned turn=0;turn<100;turn++){now+=20;publish_error=0;virtual_service(sender);assert(!json_owner&&!fixture_scope_token&&!native_config_live);}
 json_heap_snapshot(&after);assert(scope_attempts-attempts==3 && !sends);
 assert(after.guard_denied>before.guard_denied);healthy_stats();
 /* Simulate the measured quiet heap plus the recovered historical-layout
  * saving; maximal legal peer/member/domain configuration must complete. */
 capacity=libc_baseline+27724;now+=1000;mqtt_tick=now;
 for(unsigned turn=0;turn<200 && (mqtt_discovery_pending || !mqtt_online);turn++){
  publish_error=0;virtual_service(sender);now+=20;
  assert(!json_owner&&!fixture_scope_token&&!native_config_live);
 }
 assert(!mqtt_discovery_pending && mqtt_online && published_layout);
 assert(!memcmp(published_layout->members,configuration.members,sizeof(configuration.members)));
 assert(published_layout->peer_count==4 && published_layout->member_count==8);
 unsigned completed=sends;assert(completed>=4*7+8);
 free(published_layout);published_layout=NULL;healthy_stats();
 /* Each sender permits an independent full status, including discovery wire
  * overlap. This is graph exclusion/release proof, not transport timing proof. */
 configure();capacity=libc_baseline+50000;enforce_nested=true;mqtt_tick=now;
 for(unsigned turn=0;turn<200 && (mqtt_discovery_pending || !mqtt_online);turn++){
  virtual_service(sender);now+=20;
 }
 assert(!mqtt_discovery_pending && published_layout);free(published_layout);published_layout=NULL;healthy_stats();
 /* Real allocator faults across graph construction and wire growth clean all
  * ownership. Quota and transport errors preserve a retryable cursor. */
 discovery_layout_t layout;discovery_layout_capture(&layout,&configuration);enforce_nested=false;
 for(unsigned leaf=0;leaf<2;leaf++)for(unsigned point=1;point<=180;point++){
  unsigned cursor=leaf;fault_at=point;fault_calls=0;publish_error=0;mqtt_discovery_deferred=false;
  (void)discovery_virtual_step(sender,&layout,"test",false,&cursor);fault_at=0;
  assert(cursor==leaf || cursor==leaf+1);assert(!json_owner&&!fixture_scope_token);healthy_stats();
 }
 unsigned cursor=0;quota=true;quota_count=1;publish_error=0;mqtt_discovery_deferred=false;
 assert(!discovery_virtual_step(sender,&layout,"test",false,&cursor));assert(!cursor&&!mqtt_discovery_deferred&&publish_error==PH_MQTT_MORE);
 quota=false;publish_error=0;assert(!discovery_virtual_step(sender,&layout,"test",false,&cursor));assert(cursor==1);healthy_stats();
 transport_fail=true;publish_error=0;assert(!discovery_virtual_step(sender,&layout,"test",false,&cursor));assert(cursor==1&&publish_error==-17);transport_fail=false;publish_error=0;healthy_stats();
 /* Refused lease and native config OOM make no graph and release the token. */
 configure();unsigned token=service_json_claim();unsigned sent=sends;virtual_service(sender);assert(sends==sent);service_json_release(token);
 native_fail_size=sizeof(feature_config_t);virtual_service(sender);assert(!json_owner&&!fixture_scope_token&&!native_config_live);native_fail_size=0;
 healthy_stats();
 char report[256];int n=snprintf(report,sizeof(report),"actual virtual discovery native=%u history=%u saved_charge=%u pressure_attempts=3 completed=%u peak=%u live=0\n",(unsigned)sizeof(feature_config_t),(unsigned)sizeof(discovery_layout_t),(unsigned)(charge(sizeof(feature_config_t))-charge(sizeof(discovery_layout_t))),completed,(unsigned)(peak-libc_baseline));_write(1,report,n);
 return 0;
}
'''


class DiscoveryBudget(unittest.TestCase):
    def test_actual_arm_discovery_retry_exclusion_and_recovery(self):
        budget.JsonBudget().compile_arm('A1',discovery_fixture(),budget=True)

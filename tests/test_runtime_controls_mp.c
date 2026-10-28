/* Production runtime controls plus historical scenarios; no JSON library. */
#define FEATURE_HOST
#define FEATURE_DEVICE_A1
#include <arpa/inet.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/config_mp.inc"
#include "../common/engine.c"
#include "../common/service_mp.h"
static feature_config_t configuration,stored;
static feature_sample_t samples[FEATURE_MAX_PEERS];
static feature_pid_t pid;
static bool automatic_on,automatic_valid,schedule_valid,clock_ok,effective_on,automatic_immediate=true,data_valid;
static unsigned automatic_speed,effective_speed,data_fields,available,available_co2;
static int automatic_night=-1;
static feature_sleep_state_t sleep_state;
static float aggregate[4],aggregate_co2;
static uint32_t test_now;
static void reset_controller(bool);
int feature_fan_apply(unsigned,int);
static void lock(void){}static void unlock(void){}
static uint32_t now_ms(void){return test_now;}
static unsigned invalidations,commits,broadcasts;
static bool fail_save;
void service_cache_changed(unsigned mask){assert(mask&&mask<=SERVICE_CACHE_ALL);invalidations++;}
static void feature_config_get(feature_config_t*c){*c=stored;feature_config_limit_peers(c);}
static bool feature_config_save(const feature_config_t*c){if(fail_save||c->revision!=stored.revision)return false;stored=*c;stored.revision++;commits++;return true;}
static void group_config_changed(const feature_config_t*a,const feature_config_t*b){(void)a;(void)b;}
static void group_broadcast_config_mp(const char*d,const ph_value*p){assert(d&&p&&p->kind==PHV_MAP);broadcasts++;}
static bool group_manual_probe(const feature_config_t*c,const char*m){(void)c;(void)m;return false;}
static struct {void *pending;bool lan;unsigned failures;char ip[16];} group_members[8];
#include "runtime_controls.inc"
#include "mp_fixture_vectors.inc"
static ph_value obj(const char*text){ph_value p;for(unsigned i=0;i<sizeof(fixture_vectors)/sizeof(*fixture_vectors);i++)if(!strcmp(text,fixture_vectors[i].text)){assert(phv_parse(PH_WIRE_MSGPACK,fixture_vectors[i].bytes,fixture_vectors[i].length,&p));return p;}fprintf(stderr,"missing fixture %s\n",text);abort();}
static void transaction(const char*d,const char*text,unsigned status){ph_value p=obj(text);ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));unsigned result=feature_domain_set_mp(d,&p,&w);if(result!=status){fprintf(stderr,"%s %s expected %u got %u\n",d,text,status,result);abort();}ph_buf*b=phw_take(&w);assert(b);ph_value r,v;int64_t code;assert(phv_parse(PH_WIRE_MSGPACK,b->buf,b->len,&r)&&phv_get(&r,"code",&v)&&phv_int(&v,&code)&&code==status);ph_buf_free(b);}
static unsigned select_effect(unsigned effect,bool output){ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));unsigned result=feature_light_select_mp(effect,output?&w:NULL);if(output){ph_buf*b=phw_take(&w);assert(b);ph_buf_free(b);}else phw_discard(&w);return result;}
static void encoded_speed_default(void){ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));assert(feature_config_encode_mp(&stored,"light",&w));ph_buf*b=phw_take(&w);assert(b);ph_value r,v;int64_t speed;assert(phv_parse(PH_WIRE_MSGPACK,b->buf,b->len,&r)&&phv_get(&r,"speed",&v)&&phv_int(&v,&speed)&&speed==5);ph_buf_free(b);}
#include "runtime_control_scenarios.inc"
static void transaction_boundaries(void){
 transaction("binding","{\"peers\":[{\"mac\":\"bad\"}]}",400);assert(!stored.peer_count);
 transaction("binding","{\"group\":\"bad/#\"}",400);
 transaction("binding","{\"peers\":[{\"mac\":\"112233445566\",\"ip\":\"garbage\"}]}",400);
 transaction("linkage","{\"hysteresis_enabled\":1,\"on_pm\":35,\"target_pm\":35}",400);
 transaction("linkage","{\"hysteresis_enabled\":1,\"metrics\":3,\"on_pm\":50,\"target_pm\":35,\"on_hcho\":0.08,\"target_hcho\":0.08}",400);
 transaction("linkage","{\"hysteresis_enabled\":1,\"metrics\":1,\"on_pm\":50,\"target_pm\":35}",200);
 transaction("linkage","{\"hysteresis_enabled\":2}",400);
 transaction("linkage","{\"metrics\":4,\"target_co2\":1000,\"on_co2\":900}",400);
 transaction("linkage","{\"metrics\":4,\"low_co2\":2000,\"high_co2\":800}",400);
 transaction("linkage","{\"metrics\":4,\"on_co2\":1500,\"target_co2\":1000}",200);
 transaction("linkage","{\"metrics\":1}",200);transaction("linkage","{\"hysteresis_enabled\":0}",200);
 transaction("linkage","{\"target_pm\":0}",400);transaction("linkage","{\"kp\":true}",400);
 transaction("linkage","{\"enabled\":1}",200);transaction("linkage","{\"enabled\":0}",200);transaction("linkage","{\"kp\":30}",200);
 transaction("light","{\"segments\":5}",400);transaction("light","{\"segment\":[{\"minimum\":10,\"maximum\":5}]}",400);
 transaction("sleep","{\"windows\":[{\"start\":120,\"end\":120}]}",400);
 transaction("schedule","{\"events\":[{\"days\":127,\"minute\":480,\"on\":1,\"speed\":60}]}",200);
 transaction("time","{\"timezone_minutes\":841}",400);transaction("time","{\"timezone_minutes\":480}",200);transaction("time","{\"revision\":0}",409);
}

static ph_value get(ph_value p,const char*k){ph_value v;assert(phv_get(&p,k,&v));return v;}
static double number(ph_value p){double n;assert(phv_number(&p,&n));return n;}
static ph_buf *average(const feature_sample_t *samples,unsigned count,uint32_t now){ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,NULL,false);feature_average_fields_mp(&w,samples,count,now);phw_end(&w);ph_buf*b=phw_take(&w);assert(b);return b;}
static ph_value parse_wire(ph_buf*b){ph_value p;assert(phv_parse(PH_WIRE_MSGPACK,b->buf,b->len,&p));return p;}
static void average_snapshots(void){
 feature_sample_t readings[3]={0};test_now=1000;
 for(unsigned i=0;i<2;i++){readings[i].valid=true;readings[i].received_ms=test_now;readings[i].value[0]=20+10*i;readings[i].value[1]=40+20*i;readings[i].value[2]=10+20*i;readings[i].value[3]=.1f+.2f*i;}
 ph_buf*b=average(readings,3,test_now);ph_value p=parse_wire(b);assert(number(get(p,"sample_count"))==2&&number(get(p,"temperature"))==25&&number(get(p,"pm25"))==20);ph_buf_free(b);
 readings[2].co2_supported=readings[2].co2_valid=true;readings[2].co2=1400;readings[2].co2_received_ms=test_now;
 b=average(readings,3,test_now);p=parse_wire(b);assert(number(get(p,"co2_sample_count"))==1&&number(get(p,"co2"))==1400);ph_buf_free(b);
 b=average(readings,3,test_now+180001);p=parse_wire(b);ph_value v=get(p,"valid");assert(v.kind==PHV_FALSE&&get(p,"temperature").kind==PHV_NULL);ph_buf_free(b);
 memset(readings,0,sizeof(readings));readings[0].valid=true;readings[0].partial=true;readings[0].fields=4;readings[0].received_ms=test_now;readings[0].value[2]=42;
 b=average(readings,1,test_now);p=parse_wire(b);assert(get(get(p,"fields"),"pm25").kind==PHV_TRUE&&get(get(p,"fields"),"formaldehyde").kind==PHV_FALSE&&get(p,"formaldehyde").kind==PHV_NULL);ph_buf_free(b);
 readings[0].valid=false;readings[0].co2_supported=readings[0].co2_valid=true;readings[0].co2=1800;readings[0].co2_received_ms=test_now;
 b=average(readings,1,test_now);p=parse_wire(b);assert(get(get(p,"fields"),"co2").kind==PHV_TRUE&&number(get(p,"co2"))==1800);ph_buf_free(b);
}
static unsigned binding_set(unsigned count){
 ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,NULL,false);phw_begin(&w,"peers",true);
 for(unsigned i=0;i<count;i++){char mac[13];snprintf(mac,sizeof(mac),"%012x",i+1);phw_begin(&w,NULL,false);phw_text(&w,"mac",mac);phw_end(&w);}phw_end(&w);phw_end(&w);ph_buf*b=phw_take(&w);ph_value p=parse_wire(b);assert(phw_init(&w,PH_WIRE_MSGPACK));unsigned status=feature_domain_set_mp("binding",&p,&w);ph_buf*r=phw_take(&w);assert(r);ph_buf_free(r);ph_buf_free(b);return status;
}
static void binding_limits_and_install_mapping(void){
 assert(binding_set(4)==200&&stored.peer_count==4&&configuration.peer_count==4);feature_config_t before=stored;
 assert(binding_set(5)==400&&!memcmp(&before,&stored,sizeof(stored))&&configuration.peer_count==4);
 ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));assert(feature_domain_get_mp("binding",&w));ph_buf*b=phw_take(&w);assert(b);assert(number(get(parse_wire(b),"max_peers"))==4);ph_buf_free(b);
 stored.peer_count=FEATURE_STORAGE_PEERS;feature_config_t snapshot=stored,loaded;feature_config_get(&loaded);assert(loaded.peer_count==4&&!memcmp(&snapshot,&stored,sizeof(stored)));assert(!install_config(&stored));stored=before;
 samples[0].valid=true;samples[0].value[2]=42;peers[0].authorized=true;strcpy(peers[0].ip,"10.0.0.1");feature_config_t next=stored;feature_peer_t swap=next.peers[0];next.peers[0]=next.peers[1];next.peers[1]=swap;
 assert(install_config(&next)&&samples[1].valid&&samples[1].value[2]==42&&peers[1].authorized&&!strcmp(peers[1].ip,"10.0.0.1"));
 next=stored;next.peers[1].token[0]^=1;assert(install_config(&next)&&!samples[1].valid&&!peers[1].authorized);
 before=configuration;next=stored;next.linkage.fixed_speed=99;fail_save=true;assert(!install_config(&next)&&!memcmp(&before,&configuration,sizeof(before)));fail_save=false;
}
static void internal_values(void) {
  feature_config_t saved=configuration;float previous[4];memcpy(previous,aggregate,sizeof(previous));
  float previous_co2=aggregate_co2;unsigned n=available,nc=available_co2;bool valid=data_valid;
  configuration.peer_count=2;configuration.linkage.statistic=STAT_MAX;
  available=2;available_co2=1;data_valid=true;
  aggregate[0]=29;aggregate[1]=60;aggregate[2]=123;aggregate[3]=.2;aggregate_co2=1400;
  feature_values_t v;feature_values_snapshot(&v);
  assert(v.valid && v.sample_count==2 && v.co2_sample_count==1 && v.configured_count==2);
  assert(v.statistic==STAT_MAX && v.value[2]==123 && v.co2==1400);
  available=available_co2=0;data_valid=false;feature_values_snapshot(&v);
  assert(!v.valid && !v.sample_count && !v.co2_sample_count);
  configuration=saved;memcpy(aggregate,previous,sizeof(previous));aggregate_co2=previous_co2;
  available=n;available_co2=nc;data_valid=valid;
}
int main(void){feature_defaults(&stored);configuration=stored;effective_controls();automatic_profiles();animation_speed();transaction_boundaries();average_snapshots();internal_values();binding_limits_and_install_mapping();assert(invalidations&&commits);return 0;}

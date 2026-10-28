/* Preserve the passive model's original domain and local-sample contract. */
#define FEATURE_HOST
#include <assert.h>
#include <arpa/inet.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "../common/config_mp.inc"
#include "../common/engine.c"
#include "../common/service_mp.h"
static feature_config_t configuration;
static feature_sample_t local_sample;
static unsigned available,available_co2,automatic_speed,commits,invalidations,publications;
static uint32_t mqtt_revision;
static bool data_valid,clock_ok,effective_on,mqtt_online;
static int publish_error;
static int (*active_publish)(const char*,const void*,size_t,bool);
typedef struct{char id[13],ip[16];uint32_t last_seen,last_read;bool valid,active;}sensor_session_t;
#define SENSOR_SESSION_IDLE_MS 600000
static sensor_session_t sensor_sessions[FEATURE_MAX_PEERS];
static void lock(void){}static void unlock(void){}
static uint32_t now_ms(void){return 200;}
static void feature_config_get(feature_config_t*c){*c=configuration;}
static bool install_config(feature_config_t*c){c->revision++;configuration=*c;commits++;return true;}
static void group_broadcast_config_mp(const char*d,const ph_value*p){assert(d&&p);}
void service_cache_changed(unsigned mask){assert(mask==SERVICE_CACHE_STATUS);invalidations++;}
#include "../common/feature_mp.inc"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "runtime_controls.inc"
#pragma GCC diagnostic pop
#include "mp_fixture_vectors.inc"
static void transaction(const char*d,const char*text,unsigned expected){
 for(unsigned i=0;i<sizeof(fixture_vectors)/sizeof(*fixture_vectors);i++)if(!strcmp(text,fixture_vectors[i].text)){
  ph_value p;assert(phv_parse(1,fixture_vectors[i].bytes,fixture_vectors[i].length,&p));ph_writer w;assert(phw_init(&w,1));assert(feature_domain_set_mp(d,&p,&w)==expected);ph_buf*b=phw_take(&w);assert(b);ph_buf_free(b);return;
 }assert(!"missing MP fixture");
}
static int capture(const char*t,const void*p,size_t n,bool retained){(void)t;(void)p;(void)n;(void)retained;publications++;return 0;}
int main(void){
 feature_defaults(&configuration);
 transaction("binding","{\"response_ids\":[]}",405);
 transaction("linkage","{\"enabled\":true}",405);
 transaction("light","{}",405);transaction("probe","{}",405);assert(!commits);
 transaction("time","{\"timezone_minutes\":480}",200);
 transaction("sleep","{\"windows\":[{\"days\":127,\"start\":1320,\"end\":420,\"brightness\":0}]}",200);assert(commits==2);
 ph_writer w;assert(phw_init(&w,1));assert(feature_domain_get_mp("binding",&w));ph_buf*b=phw_take(&w);assert(b);ph_value root,v;int64_t version;
 assert(phv_parse(1,b->buf,b->len,&root)&&!phv_get(&root,"response_ids",&v)&&phv_get(&root,"session_protocol",&v)&&phv_int(&v,&version)&&version==3);ph_buf_free(b);
 feature_sample_t sample={.value={20,50,100,.1f},.sequence=9,.boot=7,.received_ms=100,.valid=true};
 feature_local_sample(&sample);memset(&sample,0,sizeof(sample));assert(local_sample.sequence==9&&local_sample.value[2]==100&&invalidations==1);
 assert(!feature_mqtt_service(capture,true)&&!publications&&mqtt_online&&mqtt_revision==configuration.revision);
 local_sample.sequence++;assert(!feature_mqtt_service(capture,true)&&!publications);assert(!feature_mqtt_service(capture,false)&&!mqtt_online);
 puts("M1 passive domain restrictions, v3 binding, owned local sample and no extra MQTT producer PASS");return 0;
}

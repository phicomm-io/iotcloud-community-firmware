#include <assert.h>
#include "M1/m1.h"
#include "common/sensor_mp.h"
#define LOCAL_ID "aabbccddeeff"
static mqtt_account_t feature_account;
static mico_mutex_t namespace_lock;
static uint32_t mqtt_generation;
static bool mqtt_online,locked;
static unsigned sensor_sessions[4],offline_calls,display_resets,co2_resets,samples,metadata_calls,grants,group_calls;
static char last_id[13];static uint32_t last_boot,last_sequence;static double last_power;
static void lock(void){assert(!locked);locked=true;}
static void unlock(void){assert(locked);locked=false;}
static uint32_t now_ms(void){return 1000;}
static mqtt_account_t mqtt_snapshot(void){return feature_account;}
static uint32_t ph_mqtt_send_generation(void){return 9;}
bool feature_mac(const char*s,char out[13]){if(strlen(s)!=12||strspn(s,"0123456789abcdef")!=12)return false;strcpy(out,s);return true;}
void m1_display_source_offline(const char *model,const char *id){assert(!model);strcpy(last_id,id);offline_calls++;}
void m1_display_source_mqtt_reset(void){display_resets++;}
void m1_co2_source_reset(void){co2_resets++;}
void m1_co2_source_config_get(m1_co2_source_config_t*c){memset(c,0,sizeof(*c));}
void m1_co2_source_invalidate(const char *id){(void)id;}
void m1_co2_source_sample(const char *id,const feature_sample_t*s){(void)id;(void)s;}
void m1_display_source_metadata(const char *id,const ph_value *root){assert(root->kind==PHV_MAP);strcpy(last_id,id);metadata_calls++;}
void m1_display_source_sample(const char *id,const ph_value *root){
 ph_sensor_view view;assert(ph_sensor_view_parse(root,id,false,&view));strcpy(last_id,id);last_boot=view.boot;last_sequence=view.sequence;
 ph_value sensor,value;assert(phv_get(&view.fields,"power",&sensor)&&phv_get(&sensor,"value",&value)&&phv_number(&value,&last_power));samples++;
}
static bool feature_sensor_session_mp(const ph_value*p,const char**code){assert(p->kind==PHV_MAP);grants++;*code="accepted";return true;}
static bool group_text_source(const ph_value*p,char out[13]){ph_value v;return phv_get(p,"src",&v)&&phv_text(&v,out,13);}
static void group_input_mp(const ph_value*p,bool lan,bool retained,const char*ip){(void)p;(void)lan;(void)retained;(void)ip;group_calls++;}
static const char*topic_domain(const char*d){return !strcmp(d,"light")?"light/config":d;}
static ph_buf *domain_set_wire_mp(const char*d,const ph_value*p,int*status){(void)d;(void)p;*status=200;return NULL;}
static ph_buf *domain_get_wire_mp(const char*d){(void)d;return NULL;}
static bool enqueue_mp(const char*t,const ph_buf*b,bool retained){(void)t;(void)b;(void)retained;return true;}
static bool virtual_input(const char*t,const void*d,size_t n,bool retained){(void)t;(void)d;(void)n;(void)retained;return false;}
#include "common/runtime_sensor_mp.inc"
#include "runtime_dispatch.inc"
static ph_buf *snapshot(unsigned sequence){ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));ph_sensor_snapshot_begin(&w,"112233445566",UINT32_MAX,sequence,UINT32_MAX);ph_sensor_reading_write(&w,"power",12.5,true,100);ph_sensor_finish(&w);return phw_take(&w);}
int main(void){
 mqtt_account_t owner,other;assert(mqtt_account_init(&owner,1,"m1","display_owner",LOCAL_ID));assert(mqtt_account_init(&other,1,"m1","different_owner",LOCAL_ID));
 feature_mqtt_session(&owner);sensor_sessions[0]=17;feature_mqtt_session(&owner);assert(sensor_sessions[0]==17);assert(display_resets==2&&co2_resets==2&&mqtt_generation==9&&!mqtt_online);
 char topic[FEATURE_TOPIC_MAX],outside[FEATURE_TOPIC_MAX];assert(mqtt_aux(&owner,"sensor","112233445566/state",topic,sizeof(topic)));assert(mqtt_aux(&other,"sensor","112233445566/state",outside,sizeof(outside)));
 ph_buf*b=snapshot(UINT32_MAX);assert(b&&memchr(b->buf,0,b->len));assert(feature_mqtt_input(topic,b->buf,b->len,true));assert(!feature_mqtt_input(outside,b->buf,b->len,false));assert(!samples);
 assert(feature_mqtt_input(topic,b->buf,b->len,false));assert(samples==1&&last_boot==UINT32_MAX&&last_sequence==UINT32_MAX&&last_power==12.5&&!strcmp(last_id,"112233445566"));
 assert(feature_mqtt_input(topic,"{}",2,false));assert(samples==1);memset(b->buf,0,b->len);ph_buf_free(b);assert(last_power==12.5);
 const char*models[]={"m1","daikin","tc1","a1"};for(unsigned i=0;i<4;i++){
  assert(mqtt_aux(&owner,models[i],"112233445566/availability",topic,sizeof(topic)));assert(mqtt_aux(&other,models[i],"112233445566/availability",outside,sizeof(outside)));
  assert(feature_mqtt_input(topic,"online",6,false));assert(feature_mqtt_input(topic,"offline",7,true));assert(feature_mqtt_input(topic,"offline\n",8,false));assert(!feature_mqtt_input(outside,"offline",7,false));assert(offline_calls==i);
  assert(feature_mqtt_input(topic,"offline",7,false));assert(offline_calls==i+1&&!strcmp(last_id,"112233445566"));
 }
 assert(mqtt_aux(&owner,"sensor",LOCAL_ID "/command",topic,sizeof(topic)));ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,NULL,false);phw_text(&w,"action","sensor.session.set");phw_begin(&w,"params",false);phw_end(&w);phw_end(&w);b=phw_take(&w);assert(b);
 assert(feature_mqtt_input(topic,b->buf,b->len,true)&&!grants);assert(feature_mqtt_input(topic,b->buf,b->len,false)&&grants==1);ph_buf_free(b);
 sensor_descriptors[0].boot=44;feature_mqtt_session(&other);assert(!sensor_sessions[0]&&!sensor_descriptors[0].boot&&display_resets==3&&co2_resets==3&&!locked);
 puts("M1 runtime ingress/session: binary sensor dispatch, scoped availability, native grants, reconnect resets PASS");return 0;
}

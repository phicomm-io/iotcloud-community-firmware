#define FEATURE_HOST
#define FEATURE_DEVICE_A1
#define A1_FRESH_AIR
#include <arpa/inet.h>
#include <stdlib.h>
#include <assert.h>
typedef int OSStatus;
#include "../A1/fresh_air.h"
#include "../common/service_mp.h"
#include "../common/config_mp.inc"
#include "../common/engine.c"
#include "../common/fresh_config_mp.inc"
#include "../A1/fresh_air_defaults.c"
static fresh_config_t stored;
static unsigned commits,invalidations;
void fresh_config_get(fresh_config_t*c){*c=stored;}
const char *fresh_config_save(const fresh_config_t*c){if(c->revision!=stored.revision)return "conflict";stored=*c;stored.revision++;commits++;return "accepted";}
void service_cache_changed(unsigned mask){assert(mask==SERVICE_CACHE_ALL);invalidations++;}
#include "../A1/fresh_air_mp.inc"
static ph_buf *call(const char*action,ph_buf*b,const char**code){ph_value p;assert(phv_parse(PH_WIRE_MSGPACK,b->buf,b->len,&p));ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));assert(fresh_mp(action,&p,&w,code));ph_buf*r=phw_take(&w);assert(r);ph_value v;assert(phv_parse(PH_WIRE_MSGPACK,r->buf,r->len,&v));return r;}
int main(void){
 fresh_defaults(&stored);const char*code;ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,NULL,false);phw_bool(&w,"on",true);phw_uint(&w,"speed",100);phw_end(&w);ph_buf*p=phw_take(&w),*r=call("fresh_air.set",p,&code);assert(!strcmp(code,"accepted")&&stored.on&&stored.speed==100&&commits==1);ph_buf_free(p);ph_buf_free(r);
 assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,NULL,false);phw_uint(&w,"revision",2);phw_number(&w,"pwm_min",90);phw_number(&w,"pwm_max",10);phw_end(&w);p=phw_take(&w);r=call("fresh_air.config.set",p,&code);assert(!strcmp(code,"invalid_params")&&commits==1);ph_buf_free(p);ph_buf_free(r);
 assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,NULL,false);phw_uint(&w,"revision",1);phw_uint(&w,"coupling",2);phw_end(&w);p=phw_take(&w);r=call("fresh_air.config.set",p,&code);assert(!strcmp(code,"conflict")&&commits==1);ph_buf_free(p);ph_buf_free(r);
 assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,NULL,false);phw_uint(&w,"revision",2);phw_uint(&w,"max_speed",90);phw_uint(&w,"fallback_speed",0);phw_end(&w);p=phw_take(&w);r=call("fresh_air.linkage.set",p,&code);assert(!strcmp(code,"accepted")&&stored.linkage.max_speed==90&&stored.fallback_speed==0&&commits==2);ph_buf_free(p);ph_buf_free(r);
 assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,NULL,false);phw_uint(&w,"revision",3);phw_text(&w,"unknown","x");phw_end(&w);p=phw_take(&w);r=call("fresh_air.linkage.set",p,&code);assert(!strcmp(code,"invalid_params")&&commits==2);ph_buf_free(p);ph_buf_free(r);
 assert(invalidations==2);return 0;
}

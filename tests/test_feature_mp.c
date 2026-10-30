#define FEATURE_HOST
#define FEATURE_DEVICE_A1
#include <arpa/inet.h>
#include <stdlib.h>
#include <assert.h>
#include "../common/config_mp.inc"
#include "../common/engine.c"
static feature_config_t configuration;
static feature_sample_t samples[FEATURE_MAX_PEERS];
static struct {bool authorized,lan,pending;char ip[16];unsigned recover_at,sent,failures;} peers[FEATURE_MAX_PEERS];
static struct {void *pending;bool lan;unsigned failures;char ip[16];} group_members[8];
static unsigned available,available_co2,automatic_speed,commits,broadcasts;
static bool clock_ok,data_valid,effective_on,save_failure,save_race;
static void lock(void){}static void unlock(void){}
static uint32_t now_ms(void){return 10000;}
static void feature_config_get(feature_config_t *c){*c=configuration;}
static bool install_config(feature_config_t*c){if(save_race){configuration.revision++;return false;}if(save_failure||c->revision!=configuration.revision)return false;c->revision++;configuration=*c;commits++;return true;}
static void group_broadcast_config_mp(const char*d,const ph_value*p){assert(d&&p&&p->kind==PHV_MAP);broadcasts++;}
static bool group_manual_probe(const feature_config_t*c,const char*m){(void)c;(void)m;return false;}
int feature_light_select_mp(unsigned effect,ph_writer*w){(void)effect;phw_begin(w,NULL,false);phw_end(w);return 200;}
#include "../common/feature_mp.inc"
static ph_buf *params(unsigned revision,unsigned speed){ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,NULL,false);phw_uint(&w,"revision",revision);phw_uint(&w,"fixed_speed",speed);phw_end(&w);return phw_take(&w);}
static ph_buf *call(const char*action,ph_buf*b,const char**code){ph_value p;assert(phv_parse(PH_WIRE_MSGPACK,b->buf,b->len,&p));ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));assert(feature_mp(action,&p,&w,code));ph_buf*r=phw_take(&w);assert(r);ph_value v;assert(phv_parse(PH_WIRE_MSGPACK,r->buf,r->len,&v));return r;}
int main(void){
 feature_defaults(&configuration);const char *code;
 ph_buf*p=params(1,70),*r=call("linkage.set",p,&code);assert(!strcmp(code,"accepted")&&commits==1&&broadcasts==1&&configuration.linkage.fixed_speed==70);ph_buf_free(r);ph_buf_free(p);
 p=params(0,80);r=call("linkage.set",p,&code);assert(!strcmp(code,"conflict")&&commits==1&&broadcasts==1);ph_buf_free(r);ph_buf_free(p);
 p=params(2,101);r=call("linkage.set",p,&code);assert(!strcmp(code,"invalid_params")&&commits==1&&configuration.linkage.fixed_speed==70);ph_buf_free(r);ph_buf_free(p);
 p=params(2,80);save_failure=true;r=call("linkage.set",p,&code);assert(!strcmp(code,"storage_error")&&commits==1&&broadcasts==1);ph_buf_free(r);ph_buf_free(p);save_failure=false;
 p=params(2,80);save_race=true;r=call("linkage.set",p,&code);assert(!strcmp(code,"conflict")&&commits==1&&broadcasts==1&&configuration.linkage.fixed_speed==70);ph_buf_free(r);ph_buf_free(p);save_race=false;
 ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,NULL,false);phw_end(&w);p=phw_take(&w);
 const char*gets[]={"binding.get","night.get","time.get","linkage.get","light_effect.get","binding.status.get","sensors.average.get"};
 for(unsigned i=0;i<sizeof(gets)/sizeof(gets[0]);i++){r=call(gets[i],p,&code);assert(!strcmp(code,"ok"));ph_buf_free(r);}ph_buf_free(p);
 assert(feature_ntp_set_mp("clock.example",&code)&&!strcmp(configuration.ntp_host,"clock.example"));assert(!feature_ntp_set_mp("bad\nhost",&code));return 0;
}

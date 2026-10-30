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
static unsigned config_live;
static bool require_released;
static void *snapshot_malloc(size_t n){void *p=malloc(n);if(p)config_live++;return p;}
static void snapshot_free(void *p){if(p){assert(config_live);config_live--;free(p);}}
#define malloc snapshot_malloc
#define free snapshot_free
#include "../common/feature_mp.inc"
#undef malloc
#undef free
/* Model the protected wire heap independently from the frozen config copy. */
static size_t wire_live,wire_limit=(size_t)-1;
void *ph_wire_malloc(size_t n){
 assert(!require_released||!config_live);
 if(n>wire_limit-wire_live)return NULL;
 size_t *p=malloc(sizeof(*p)+n);if(!p)return NULL;*p=n;wire_live+=n;return p+1;
}
void ph_wire_free(void *ptr){if(ptr){size_t *p=(size_t*)ptr-1;assert(wire_live>=*p);wire_live-=*p;free(p);}}
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
 /* Reads must succeed when exact output fits but geometric growth does not.
  * Compare complete bytes with an independently emitted normal-capacity copy. */
 const char *domains[]={"binding","linkage","light","sleep","time","status","average"};
 for(unsigned i=0;i<sizeof(domains)/sizeof(domains[0]);i++){
  ph_writer expected;assert(phw_init(&expected,PH_WIRE_MSGPACK));
  assert(feature_domain_get_mp(domains[i],&expected));ph_buf *full=phw_take(&expected);assert(full);
  ph_writer limited;assert(phw_init(&limited,PH_WIRE_MSGPACK));
  wire_limit=wire_live+full->len+1;require_released=true;
  assert(feature_domain_get_mp(domains[i],&limited));
  ph_buf *got=phw_take(&limited);assert(got&&got->len==full->len&&!memcmp(got->buf,full->buf,got->len));
  wire_limit=(size_t)-1;require_released=false;ph_buf_free(got);ph_buf_free(full);assert(!wire_live);
 }
 /* A fully populated binding exceeds the fast buffer and must not truncate. */
 configuration.peer_count=FEATURE_MAX_PEERS;configuration.member_count=8;
 memset(configuration.group,'g',sizeof(configuration.group)-1);configuration.group[sizeof(configuration.group)-1]=0;
 for(unsigned i=0;i<configuration.peer_count;i++){
  feature_peer_t *peer=&configuration.peers[i];strcpy(peer->id,"aabbccddeeff");strcpy(peer->ip,"192.168.100.100");
  memset(peer->group,'g',sizeof(peer->group)-1);peer->group[sizeof(peer->group)-1]=0;
 }
 for(unsigned i=0;i<configuration.member_count;i++){strcpy(configuration.members[i].id,"aabbccddeeff");strcpy(configuration.members[i].ip,"192.168.100.100");}
 ph_writer direct,large;assert(phw_init(&direct,PH_WIRE_MSGPACK));assert(feature_domain_config_mp(&configuration,"binding",&direct));
 ph_buf *expected_large=phw_take(&direct);assert(expected_large&&expected_large->len>1024);
 assert(phw_init(&large,PH_WIRE_MSGPACK));assert(feature_domain_get_mp("binding",&large));
 ph_buf *got_large=phw_take(&large);assert(got_large&&got_large->len==expected_large->len&&!memcmp(got_large->buf,expected_large->buf,got_large->len));
 ph_buf_free(got_large);ph_buf_free(expected_large);assert(!wire_live);
 assert(feature_ntp_set_mp("clock.example",&code)&&!strcmp(configuration.ntp_host,"clock.example"));assert(!feature_ntp_set_mp("bad\nhost",&code));return 0;
}

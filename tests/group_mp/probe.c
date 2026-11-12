#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include "typed_wire.h"
#include "common/lan_wire.inc"
#define FEATURE_DEVICE_A1
#define FEATURE_WIRE_MAX 1200
#define FEATURE_PORT 10000
#define FEATURE_TOPIC_MAX 256
#define LOCAL_ID localid
static char localid[13]="001122334455";
typedef struct {char id[13],ip[16];} member_t;
typedef struct {char group[49];uint8_t group_token[16];member_t members[8];unsigned member_count;bool group_force_mqtt,group_recovery;uint32_t group_timeout_ms,group_recovery_seconds;} feature_config_t;
typedef struct {char key[20];} mqtt_account_t;
static feature_config_t config;
static uint32_t boot_id=777,member_seen[8],mqtt_generation=1,ticks=1000,serial=100;
static unsigned wire_calls,wire_fail_at,runtime_calls,runtime_fail_at,udp_sends;
static int last_mqtt_speed=-1,last_udp_speed=-1;
void *ph_wire_malloc(size_t n){wire_calls++;if(wire_fail_at&&wire_calls==wire_fail_at)return NULL;return malloc(n);}
void ph_wire_free(void *p){free(p);}
static void *group_runtime_malloc(size_t n){runtime_calls++;if(runtime_fail_at&&runtime_calls==runtime_fail_at)return NULL;return malloc(n);}
static int udp=-1,namespace_lock,controls,mqtt_sends;static bool allow_control=true;
static mqtt_account_t feature_account={"test"};
static void lock(void){}static void unlock(void){}
static void mico_rtos_lock_mutex(int *m){(void)m;}static void mico_rtos_unlock_mutex(int *m){(void)m;}
static uint32_t now_ms(void){return ticks;}static uint32_t next_request(void){return ++serial;}
static void feature_config_get(feature_config_t *c){*c=config;}
static bool install_config(const feature_config_t *c){config=*c;return true;}
static bool feature_is_coordinator(void){return false;}
static void new_token(uint8_t *t){memset(t,7,16);}
static bool feature_lan_auth(const char *p,const uint8_t *t,unsigned s){(void)p;(void)t;(void)s;return false;}
static bool feature_control_mp(const char *kind,const ph_value *body,const char **code){(void)kind;(void)body;(void)code;controls++;return allow_control;}
static bool feature_config_decode_mp(feature_config_t *c,const char *d,const ph_value *v){(void)c;(void)d;(void)v;return true;}
static mqtt_account_t mqtt_snapshot(void){return feature_account;}
static int mqtt_format(char *out,size_t cap,const char *fmt,const char *key){return snprintf(out,cap,fmt,key);}
static bool mqtt_link_topic(const mqtt_account_t *a,const char *model,const char *dst,const char *src,const char *op,char *out,size_t cap){(void)model;(void)dst;(void)src;(void)op;snprintf(out,cap,"phicomm/v2/%s/test",a->key);return true;}
static int ph_mqtt_enqueue_send_generation(const char *t,const void *p,size_t n,unsigned qos,bool retained,uint32_t gen){(void)t;ph_value root,body,speed;int64_t value;assert(p&&n&&phv_parse(PH_WIRE_MSGPACK,p,n,&root)&&root.kind==PHV_MAP);if(phv_get(&root,"body",&body)&&phv_get(&body,"speed",&speed)&&phv_int(&speed,&value))last_mqtt_speed=(int)value;(void)qos;(void)retained;(void)gen;mqtt_sends++;return 0;}
static bool local_ready;static uint8_t last_sent_token;
static bool lan_client_token(const char *p,const char *ip,unsigned scopes,uint8_t out[16]){(void)p;(void)ip;(void)scopes;if(local_ready)memset(out,0xaa,16);return local_ready;}
static void lan_client_invalidate(const char *peer){(void)peer;}
static bool fast_ack;
static ssize_t simulated_sendto(int,const void *,size_t,int,const struct sockaddr *,socklen_t);
#define sendto simulated_sendto
#define malloc group_runtime_malloc
#include "common/group_mp.inc"
#undef malloc
static ph_buf *packet(uint32_t boot,uint32_t id,const char *op){
 char saved[13];strcpy(saved,localid);strcpy(localid,"aabbccddeeff");strcpy(config.members[0].id,saved);
 uint32_t savedboot=boot_id;boot_id=boot;
 ph_buf *wire=group_envelope_mp(&config,0,op,id,NULL,"fan",NULL);
 boot_id=savedboot;strcpy(localid,saved);strcpy(config.members[0].id,"aabbccddeeff");return wire;
}
static ssize_t simulated_sendto(int fd,const void *data,size_t n,int flags,const struct sockaddr *dst,socklen_t len){
 (void)fd;(void)flags;(void)dst;(void)len;udp_sends++;
 ph_value request;uint32_t id;assert(phv_parse(PH_WIRE_MSGPACK,data,n,&request));assert(lan_u32(&request,"id",&id));
 ph_value body,speed;int64_t value;if(phv_get(&request,"body",&body)&&phv_get(&body,"speed",&speed)&&phv_int(&speed,&value))last_udp_speed=(int)value;
 ph_value tv;const void *token;size_t token_bytes;assert(phv_get(&request,"token",&tv)&&phv_bytes(&tv,&token,&token_bytes)&&token_bytes==16);last_sent_token=((const uint8_t*)token)[0];
 if(fast_ack){uint8_t saved_token[16];memcpy(saved_token,config.group_token,16);memcpy(config.group_token,token,16);ph_buf *ack=packet(2,id,"group_ack");memcpy(config.group_token,saved_token,16);ph_value v;assert(phv_parse(PH_WIRE_MSGPACK,ack->buf,ack->len,&v));assert(group_input_mp(&v,true,false,"10.0.0.2"));ph_buf_free(ack);}
 return (ssize_t)n;
}
static void receive(uint32_t boot,uint32_t id){ph_buf *b=packet(boot,id,"group_control");assert(b);ph_value v;assert(phv_parse(PH_WIRE_MSGPACK,b->buf,b->len,&v));assert(group_input_mp(&v,false,false,NULL));ph_buf_free(b);}
static ph_value body_view(unsigned char storage[64],unsigned speed){
 ph_buf buf;ph_writer w;assert(phw_init_fixed(&w,PH_WIRE_MSGPACK,&buf,storage,64));phw_begin(&w,NULL,false);phw_uint(&w,"speed",speed);phw_end(&w);size_t n=phw_finish_fixed(&w);ph_value view;assert(phv_parse(PH_WIRE_MSGPACK,storage,n,&view));return view;
}
static void assert_quiet(unsigned mqtt_before,unsigned udp_before){assert((unsigned)mqtt_sends==mqtt_before&&udp_sends==udp_before&&!group_members[0].pending);}
static void allocation_failures(void){
 unsigned char storage[64];ph_value body=body_view(storage,60);local_ready=false;fast_ack=false;udp=1;
 assert(!group_send_wire("10.0.0.2",NULL));group_mqtt_mp(0,&config,NULL);
 for(unsigned mqtt_only=0;mqtt_only<2;mqtt_only++){
  config.group_force_mqtt=mqtt_only;group_members[0].lan=true;
  /* Both object-header and backing-buffer root allocation failures are quiet. */
  for(unsigned fail=1;fail<=2;fail++){
   unsigned mqtt_before=mqtt_sends,udp_before=udp_sends;wire_calls=0;wire_fail_at=fail;
   assert(feature_group_control_mp("fan",&body));assert(wire_calls>=fail);assert_quiet(mqtt_before,udp_before);
   wire_calls=0;group_broadcast_config_mp("light",&body);assert(wire_calls>=fail);assert_quiet(mqtt_before,udp_before);
  }
  wire_fail_at=0;runtime_calls=0;runtime_fail_at=1;unsigned mqtt_before=mqtt_sends,udp_before=udp_sends;
  assert(!feature_group_control_mp("fan",&body));assert_quiet(mqtt_before,udp_before);runtime_fail_at=0;
 }
 /* Failure after a root already exists must discard the whole packet. */
 unsigned char large_storage[768],padding[400];memset(padding,0xab,sizeof(padding));ph_buf large_buf;ph_writer large_writer;
 assert(phw_init_fixed(&large_writer,PH_WIRE_MSGPACK,&large_buf,large_storage,sizeof(large_storage)));phw_begin(&large_writer,NULL,false);phw_uint(&large_writer,"speed",60);phw_bytes_value(&large_writer,"padding",padding,sizeof(padding));phw_end(&large_writer);size_t large_length=phw_finish_fixed(&large_writer);ph_value large_body;assert(phv_parse(PH_WIRE_MSGPACK,large_storage,large_length,&large_body));
 for(unsigned mqtt_only=0;mqtt_only<2;mqtt_only++){
  config.group_force_mqtt=mqtt_only;group_members[0].lan=true;unsigned mqtt_before=mqtt_sends,udp_before=udp_sends;wire_calls=0;wire_fail_at=3;
  assert(feature_group_control_mp("fan",&large_body));assert(wire_calls>=3);assert_quiet(mqtt_before,udp_before);
  wire_calls=0;group_broadcast_config_mp("light",&large_body);assert(wire_calls>=3);assert_quiet(mqtt_before,udp_before);
 }
 config.group_force_mqtt=false;group_members[0].lan=false;group_members[0].joined=true;group_members[0].retry_at=0;
 for(unsigned fail=1;fail<=2;fail++){unsigned mqtt_before=mqtt_sends,udp_before=udp_sends;wire_calls=0;wire_fail_at=fail;group_poll(&config,ticks);assert(wire_calls>=fail);assert_quiet(mqtt_before,udp_before);}
 wire_fail_at=0;
 /* Retry-copy failures may send a complete MQTT fallback, never a partial root. */
 for(unsigned fail=3;fail<=4;fail++){
  group_members[0].lan=true;wire_calls=0;wire_fail_at=fail;int before=mqtt_sends;unsigned udp_before=udp_sends;last_mqtt_speed=-1;
  assert(feature_group_control_mp("fan",&body));assert(mqtt_sends==before+1&&udp_sends==udp_before&&last_mqtt_speed==60&&!group_members[0].pending);
 }
 wire_fail_at=0;
}
static void source_lifetime_and_two_probe_recovery(void){
 unsigned char *storage=malloc(64);assert(storage);ph_value body=body_view(storage,60);udp=1;fast_ack=false;local_ready=false;group_members[0].lan=true;group_members[0].joined=true;
 assert(feature_group_control_mp("fan",&body));assert(group_members[0].pending&&last_udp_speed==60);
 body=body_view(storage,99);(void)body;memset(storage,0xee,64);free(storage);
 ph_value root,pending_body,speed;int64_t value;assert(phv_parse(PH_WIRE_MSGPACK,group_members[0].pending->buf,group_members[0].pending->len,&root)&&phv_get(&root,"body",&pending_body)&&phv_get(&pending_body,"speed",&speed)&&phv_int(&speed,&value)&&value==60);
 unsigned failures=group_members[0].failures;int before=mqtt_sends;ticks+=config.group_timeout_ms+1;group_poll(&config,ticks);
 assert(!group_members[0].pending&&!group_members[0].lan&&group_members[0].failures==failures+1&&mqtt_sends==before+1&&last_mqtt_speed==60);
 group_members[0].retry_at=0;group_members[0].successes=0;fast_ack=true;
 group_poll(&config,ticks);assert(!group_members[0].pending&&!group_members[0].lan&&group_members[0].successes==1);
 group_poll(&config,ticks);assert(!group_members[0].pending&&group_members[0].lan&&group_members[0].successes==2);
 fast_ack=false;udp=-1;
}
int main(void){
 strcpy(config.group,"test");config.member_count=1;strcpy(config.members[0].ip,"10.0.0.2");strcpy(config.members[0].id,"aabbccddeeff");memset(config.group_token,7,16);config.group_timeout_ms=100;config.group_recovery_seconds=1;
 receive(1,1);assert(controls==1);receive(1,1);assert(controls==1);
 allow_control=false;receive(1,2);assert(controls==2);allow_control=true;receive(1,2);assert(controls==3);receive(1,2);assert(controls==3);
 receive(2,1);assert(controls==4);receive(1,3);assert(controls==4); /* retired boot cannot revive */
 group_members[0].lan=true;ph_buf *b=packet(2,10,"group_control");group_send_mp(&config,0,b,10);ph_buf_free(b);assert(!group_members[0].pending); /* failed UDP frees owned slot + MQTT fallback */
 b=packet(2,11,"group_control");group_members[0].pending=b;group_members[0].sent=ticks;group_members[0].joined=true;group_members[0].lan=true;ticks+=101;int before=mqtt_sends;group_poll(&config,ticks);assert(!group_members[0].pending&&group_members[0].failures==1&&mqtt_sends==before+1);
 /* ACK is delivered synchronously inside sendto and frees the pending copy. */
 udp=1;fast_ack=true;group_members[0].lan=true;b=packet(2,30,"group_control");group_send_mp(&config,0,b,30);ph_buf_free(b);assert(!group_members[0].pending);fast_ack=false;
 local_ready=true;fast_ack=true;group_members[0].lan=true;b=packet(2,31,"group_control");group_send_mp(&config,0,b,31);ph_buf_free(b);assert(last_sent_token==0xaa&&!group_members[0].pending);assert(config.group_token[0]==7);local_ready=false;fast_ack=false;udp=-1;
 /* Async pending keeps independent bytes after borrowed input is destroyed. */
 b=packet(2,12,"group_control");group_members[0].pending=ph_buf_new(b->len+1);memcpy(group_members[0].pending->buf,b->buf,b->len);group_members[0].pending->len=b->len;ph_buf_free(b);assert(phv_parse(PH_WIRE_MSGPACK,group_members[0].pending->buf,group_members[0].pending->len,&(ph_value){0}));
 feature_config_t changed=config;strcpy(changed.group,"other");group_config_changed(&config,&changed);assert(!group_members[0].pending);
 allocation_failures();source_lifetime_and_two_probe_recovery();
 puts("group MP replay/retry/ownership/OOM/two-probe PASS");return 0;
}

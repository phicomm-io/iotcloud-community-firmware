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
static int ph_mqtt_enqueue_send_generation(const char *t,const void *p,size_t n,unsigned qos,bool retained,uint32_t gen){(void)t;(void)p;(void)n;(void)qos;(void)retained;(void)gen;mqtt_sends++;return 0;}
static bool fast_ack;
static ssize_t simulated_sendto(int,const void *,size_t,int,const struct sockaddr *,socklen_t);
#define sendto simulated_sendto
#include "common/group_mp.inc"
static ph_buf *packet(uint32_t boot,uint32_t id,const char *op){
 char saved[13];strcpy(saved,localid);strcpy(localid,"AABBCCDDEEFF");strcpy(config.members[0].id,saved);
 uint32_t savedboot=boot_id;boot_id=boot;
 ph_buf *wire=group_envelope_mp(&config,0,op,id,NULL,"fan",NULL);
 boot_id=savedboot;strcpy(localid,saved);strcpy(config.members[0].id,"AABBCCDDEEFF");return wire;
}
static ssize_t simulated_sendto(int fd,const void *data,size_t n,int flags,const struct sockaddr *dst,socklen_t len){
 (void)fd;(void)flags;(void)dst;(void)len;
 ph_value request;uint32_t id;assert(phv_parse(PH_WIRE_MSGPACK,data,n,&request));assert(lan_u32(&request,"id",&id));
 if(fast_ack){ph_buf *ack=packet(2,id,"group_ack");ph_value v;assert(phv_parse(PH_WIRE_MSGPACK,ack->buf,ack->len,&v));assert(group_input_mp(&v,true,false,"10.0.0.2"));ph_buf_free(ack);}
 return (ssize_t)n;
}
static void receive(uint32_t boot,uint32_t id){ph_buf *b=packet(boot,id,"group_control");assert(b);ph_value v;assert(phv_parse(PH_WIRE_MSGPACK,b->buf,b->len,&v));assert(group_input_mp(&v,false,false,NULL));ph_buf_free(b);}
int main(void){
 strcpy(config.group,"test");config.member_count=1;strcpy(config.members[0].id,"AABBCCDDEEFF");memset(config.group_token,7,16);config.group_timeout_ms=100;config.group_recovery_seconds=1;
 receive(1,1);assert(controls==1);receive(1,1);assert(controls==1);
 allow_control=false;receive(1,2);assert(controls==2);allow_control=true;receive(1,2);assert(controls==3);receive(1,2);assert(controls==3);
 receive(2,1);assert(controls==4);receive(1,3);assert(controls==4); /* retired boot cannot revive */
 group_members[0].lan=true;ph_buf *b=packet(2,10,"group_control");group_send_mp(&config,0,b,10);ph_buf_free(b);assert(!group_members[0].pending); /* failed UDP frees owned slot + MQTT fallback */
 b=packet(2,11,"group_control");group_members[0].pending=b;group_members[0].sent=ticks;group_members[0].joined=true;group_members[0].lan=true;ticks+=101;int before=mqtt_sends;group_poll(&config,ticks);assert(!group_members[0].pending&&group_members[0].failures==1&&mqtt_sends==before+1);
 /* ACK is delivered synchronously inside sendto and frees the pending copy. */
 udp=1;fast_ack=true;group_members[0].lan=true;b=packet(2,30,"group_control");group_send_mp(&config,0,b,30);ph_buf_free(b);assert(!group_members[0].pending);fast_ack=false;udp=-1;
 /* Async pending keeps independent bytes after borrowed input is destroyed. */
 b=packet(2,12,"group_control");group_members[0].pending=ph_buf_new(b->len+1);memcpy(group_members[0].pending->buf,b->buf,b->len);group_members[0].pending->len=b->len;ph_buf_free(b);assert(phv_parse(PH_WIRE_MSGPACK,group_members[0].pending->buf,group_members[0].pending->len,&(ph_value){0}));
 feature_config_t changed=config;strcpy(changed.group,"other");group_config_changed(&config,&changed);assert(!group_members[0].pending);
 puts("group MP replay/retry/ownership PASS");return 0;
}

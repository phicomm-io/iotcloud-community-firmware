#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <arpa/inet.h>
#define FEATURE_HOST
#include "common/phicomm_features.h"
#include "common/lan_wire.inc"
#define LOCAL_ID "aabbccddeeff"
#define SENSOR_SESSION_IDLE_MS 1800000u
typedef struct {char id[13],ip[16];uint8_t token[16];uint32_t boot,last_seen,last_read;bool valid,active;} sensor_session_t;
static sensor_session_t sensor_sessions[4];static feature_sample_t local_sample;
static mqtt_account_t feature_account;static unsigned namespace_lock,mqtt_generation;
static uint32_t ticks=100,boot_id=7;static bool locked,mqtt_online;static unsigned replies;static int udp=1;static lan_packet last_reply;
static uint32_t now_ms(void){return ticks;}static void lock(void){assert(!locked);locked=true;}static void unlock(void){assert(locked);locked=false;}
static void mico_rtos_lock_mutex(unsigned *m){assert(!*m);*m=1;}static void mico_rtos_unlock_mutex(unsigned *m){assert(*m);*m=0;}
static uint32_t ph_mqtt_send_generation(void){return ++mqtt_generation;}
static void sensor_descriptors_reset(void){}static void m1_co2_source_reset(void){}static void m1_display_source_mqtt_reset(void){}
static bool feature_lan_auth(const char *peer,const uint8_t token[16],unsigned scopes){(void)peer;(void)token;(void)scopes;return false;}
static ssize_t capture_sendto(int fd,const void *data,size_t n,int flags,const struct sockaddr *address,socklen_t length){(void)fd;(void)flags;(void)address;(void)length;assert(lan_decode(data,n,&last_reply));replies++;return n;}
#define sendto capture_sendto
#include "runtime.inc"
static bool grant_peer(const char *peer,uint32_t boot,const uint8_t token[16]){
 char storage[256];ph_writer w;ph_buf buf;assert(phw_init_fixed(&w,PH_WIRE_MSGPACK,&buf,storage,sizeof(storage)));phw_begin(&w,NULL,false);phw_uint(&w,"v",3);phw_text(&w,"a1",peer);phw_u32(&w,"boot",boot);phw_bytes_value(&w,"token",token,16);phw_end(&w);size_t n=phw_finish_fixed(&w);ph_value params;assert(phv_parse(PH_WIRE_MSGPACK,storage,n,&params));const char *code;bool ok=feature_sensor_session_mp(&params,&code);memset(storage,0,sizeof(storage));return ok;
}
static void readiness(bool expected,uint32_t expected_idle){
 char storage[512];ph_buf buf;ph_writer w;assert(phw_init_fixed(&w,PH_WIRE_MSGPACK,&buf,storage,sizeof(storage)));status_peers(&w);size_t n=phw_finish_fixed(&w);ph_value peers,peer,v;bool ready;int64_t idle;
 assert(phv_parse(PH_WIRE_MSGPACK,storage,n,&peers)&&peers.count==1&&phv_at(&peers,0,&peer)&&phv_get(&peer,"session_ready",&v)&&phv_bool(&v,&ready)&&ready==expected&&phv_get(&peer,"idle_ms",&v)&&phv_int(&v,&idle)&&idle==expected_idle);
}
int main(void){
 uint8_t token[16];memset(token,0xab,16);lan_packet packet={0};packet.op=LAN_READ;strcpy(packet.src,"112233445566");strcpy(packet.dst,LOCAL_ID);packet.boot=11;packet.id=7;memcpy(packet.token,token,16);
 assert(!feature_bound());lan_sensor_input(&packet,"127.0.0.1");assert(!feature_bound()&&!replies);
 assert(grant_peer(packet.src,UINT32_MAX,token)&&sensor_sessions[0].boot==UINT32_MAX);assert(grant_peer(packet.src,11,token)&&feature_bound());assert(grant_peer(packet.src,11,token));readiness(false,0);
 local_sample=(feature_sample_t){.value={20,50,100,.1},.sequence=9,.boot=7,.received_ms=100,.valid=true};ticks=200;
 for(unsigned i=0;i<32;i++){packet.op=i%2?LAN_PROBE:LAN_READ;lan_sensor_input(&packet,"127.0.0.1");assert(replies==i+1&&last_reply.id==7&&last_reply.op==(i%2?LAN_PONG:LAN_SAMPLE));if(!(i%2))assert(last_reply.sample.sequence==9);}
 uint32_t seen=sensor_sessions[0].last_seen;unsigned before=replies;ticks++;
 packet.boot=10;lan_sensor_input(&packet,"127.0.0.1");packet.boot=11;packet.token[0]^=1;lan_sensor_input(&packet,"127.0.0.1");packet.token[0]^=1;
 packet.op=LAN_SAMPLE;lan_sensor_input(&packet,"127.0.0.1");packet.op=LAN_PROBE;assert(sensor_sessions[0].last_seen==seen&&replies==before);
 ticks=seen+FEATURE_STALE_MS;assert(grant_peer(packet.src,11,token));readiness(false,FEATURE_STALE_MS);
 lan_sensor_input(&packet,"127.0.0.1");readiness(true,0);seen=ticks;
 ticks=seen+SENSOR_SESSION_IDLE_MS;assert(!feature_bound());before=replies;lan_sensor_input(&packet,"127.0.0.1");assert(!feature_bound()&&replies==before);
 assert(grant_peer(packet.src,11,token));readiness(false,0);
 for(unsigned i=1;i<=4;i++){char peer[13];snprintf(peer,sizeof(peer),"%012x",i);assert(grant_peer(peer,1,token)==(i<4));}
 uint8_t zero[16]={0};assert(!grant_peer(packet.src,11,zero));
 mqtt_account_t account,other;assert(mqtt_account_init(&account,1,"m1","owner",LOCAL_ID));feature_account=account;feature_mqtt_session(&account);assert(feature_bound());assert(mqtt_account_init(&other,1,"m1","different",LOCAL_ID));feature_mqtt_session(&other);assert(!feature_bound());
 puts("native M1 sessions: unauthorized read, 32 read/probe, readiness, idle expiry, bounded grants and account isolation PASS");return 0;
}

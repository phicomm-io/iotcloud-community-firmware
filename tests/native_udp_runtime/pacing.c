#include <assert.h>
#include <stdlib.h>
#define FEATURE_HOST
#include "common/phicomm_features.h"
#include "common/lan_wire.inc"
#define FEATURE_DEVICE_A1
#define LOCAL_ID "aabbccddeeff"
static feature_config_t configuration;
static mqtt_account_t feature_account;
static uint32_t ticks=10000,boot_id=1,mqtt_generation=1;
static unsigned namespace_lock,queued,grants,reads,serial;
static bool locked,mqtt_online,data_valid,mqtt_discovery_pending,mqtt_discovery_deferred,mqtt_capture_deferred;
static unsigned mqtt_tick,mqtt_leaf_pending,mqtt_telemetry_cursor,data_fields,published_count;
static char published_root[16];static void *published_layout;
static feature_sample_t samples[4];static struct {ph_buf *pending;} group_members[8];
static void lock(void){assert(!locked);locked=true;}static void unlock(void){assert(locked);locked=false;}
static void mico_rtos_lock_mutex(unsigned *m){(void)m;}static void mico_rtos_unlock_mutex(unsigned *m){(void)m;}
static uint32_t now_ms(void){return ticks;}static uint32_t ph_mqtt_send_generation(void){queued=0;return ++mqtt_generation;}
static uint32_t next_request(void){return ++serial;}
static mqtt_account_t mqtt_snapshot(void){return feature_account;}
static bool new_token(uint8_t token[16]){memset(token,0xab,16);return true;}
static bool lan_client_token(const char *peer,const char *ip,unsigned scope,uint8_t token[16]){(void)peer;(void)ip;(void)scope;(void)token;return false;}
static void lan_client_invalidate(const char *peer){(void)peer;}
static void sensor_descriptors_reset(void){}
static bool lan_send(const char *ip,const lan_packet *packet){assert(!strcmp(ip,"127.0.0.1"));assert(packet->op==LAN_READ);reads++;return true;}
static int ph_mqtt_enqueue_send_generation(const char *topic,const void *data,size_t length,unsigned qos,bool retain,uint32_t generation){
 (void)topic;(void)qos;assert(!retain&&generation==mqtt_generation);ph_value root,action,params,token;const void *bytes;size_t n;
 assert(phv_parse(PH_WIRE_MSGPACK,data,length,&root)&&phv_get(&root,"action",&action)&&phv_equal_text(&action,"sensor.session.set")&&phv_get(&root,"params",&params)&&phv_get(&params,"token",&token)&&phv_bytes(&token,&bytes,&n)&&n==16);assert(((const uint8_t*)bytes)[0]==0xab);queued++;grants++;return 0;
}
#include "runtime.inc"
int main(void){
 assert(mqtt_account_init(&feature_account,1,"a1","owner",LOCAL_ID));configuration.peer_count=1;strcpy(configuration.peers[0].id,"112233445566");strcpy(configuration.peers[0].ip,"127.0.0.1");configuration.peers[0].timeout_ms=1000;configuration.peers[0].recovery=1;
 sensor_poll(&configuration,0,ticks,true);assert(queued==1&&peers[0].auth_sent&&peers[0].pending&&reads==1);
 mqtt_account_t same=mqtt_snapshot();feature_mqtt_session(&same);assert(!queued&&!peers[0].auth_sent);ticks++;sensor_poll(&configuration,0,ticks,true);assert(queued==1&&peers[0].auth_sent);queued=0;
 unsigned before=grants;
 for(unsigned i=1;i<=80;i++){ticks=10000+i*1000;peers[0].pending=false;peers[0].lan=true;peers[0].authorized=true;peers[0].failures=0;peers[0].sent=ticks;sensor_poll(&configuration,0,ticks,i<40);assert(!queued&&grants==before);}
 /* Healthy reconnect preserves the installed UDP token and remains quiet. */
 uint8_t token[16];memcpy(token,peers[0].session_token,16);feature_mqtt_session(&same);assert(peers[0].authorized&&peers[0].auth_sent&&!memcmp(token,peers[0].session_token,16));sensor_poll(&configuration,0,ticks,true);assert(!queued);
 peers[0].lan=false;peers[0].pending=false;peers[0].failures=4;peers[0].recover_at=ticks+60000;sensor_poll(&configuration,0,ticks,true);assert(queued==1);queued=0;
 for(unsigned i=1;i<60;i++){ticks+=1000;sensor_poll(&configuration,0,ticks,true);assert(!queued);}ticks+=1000;sensor_poll(&configuration,0,ticks,true);assert(queued==1);queued=0;
 mqtt_account_t other;assert(mqtt_account_init(&other,1,"a1","other",LOCAL_ID));samples[0].valid=true;feature_mqtt_session(&other);assert(!peers[0].auth_sent&&!peers[0].authorized&&!samples[0].valid&&!locked);
 puts("native A1 grant pacing: reconnect retry, 80s healthy/broker-loss quiet, 60s retry limit and account isolation PASS");return 0;
}

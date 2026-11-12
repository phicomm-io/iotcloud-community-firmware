/* Production primary snapshot, typed producer and complete session owner. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "common/mqtt_account.h"
#include "common/mqtt/publication.h"
#include "tests/sensor_mp_fixture.h"

#define MQTT_SUCCESS 0
#define PH_MQTT_MORE 2
#define PH_MQTT_RECONFIGURE 1
#define kNoErr 0
typedef int (*sender_t)(const char *,const void *,size_t,bool);
typedef struct { unsigned revision,speed;bool fan_on; } a1_config_t;
typedef struct { bool wifi_connected,led_visible; } mqtt_state_t;
typedef struct { unsigned n; } a1_filter_storage_t;
static a1_config_t cfg={1,90,true},current;
static mqtt_state_t state;
static mqtt_account_t account;
static struct { bool fan_ready,ota_active,reboot_pending; } a1_state={true,false,false};
static unsigned a1_lock,locked,lock_calls;
static bool wifi=true,connection=true,ota_active,random_failure;
static bool effective_on=true;
static unsigned effective_speed=37;
static uint32_t now_ms,random_value=UINT32_MAX;
static unsigned random_calls,attempts,sends,normal,features,snapshots,capabilities;
static int send_result,native_result,discovery_result;
static bool rediscover,sent,old_on,old_visible,filter_turn,discovery_deferred;
static uint8_t discovery_language;
static uint32_t discovery_retry_at,filter_sent_at;
static unsigned feature_revision=1,count,discovery_index=12;
static unsigned state_index,state_pending=127,state_seen,revision,old_speed;
static a1_filter_storage_t sent_filter;
static char base[1],feature_filters[1];
static ph_mqtt_publication native_publication;
static int snapshot_result;

static void mico_rtos_lock_mutex(unsigned *lock) { assert(lock==&a1_lock && !locked);locked=1;++lock_calls; }
static void mico_rtos_unlock_mutex(unsigned *lock) { assert(lock==&a1_lock && locked);locked=0; }
static uint32_t mico_rtos_get_time(void) { return now_ms; }
static int MicoRandomNumberRead(void *out,int n) {
    assert(n==(int)sizeof(random_value));++random_calls;
    if(random_failure)return -1;
    memcpy(out,&random_value,n);return kNoErr;
}
#ifdef A1_FRESH_AIR
typedef struct { unsigned primary; } fresh_snapshot_t;
static void fresh_snapshot(fresh_snapshot_t *f) { f->primary=effective_speed; }
#else
static void feature_effective_fan(bool requested_on,unsigned requested_speed,bool *on,unsigned *speed) {
    assert(requested_on==current.fan_on && requested_speed==current.speed);
    *on=effective_on;*speed=effective_speed;
}
#endif
static int publish_bytes(const char *topic,const void *body,size_t n,bool retained) {
    assert(strlen(topic)<192 && n<512 && !locked);
    if(retained){assert(fixture_sensor_output(topic,body,n,true,"fan_speed_percent","Fan speed","%",0));return 0;}
    ++attempts;fixture_sensor_output(topic,body,n,false,"fan_speed_percent","Fan speed","%",send_result);
    if(send_result)return send_result;
    ++sends;return 0;
}
static void ota_job_poll(void) {}
static int ota_service(const char *b) { assert(b==base);return MQTT_SUCCESS; }
static bool ota_job_active(void) { return ota_active; }
static void mqtt_state_get(mqtt_state_t *s) { s->wifi_connected=wifi; }
static void a1_config_get(a1_config_t *c) { *c=cfg; }
static bool same_connection(const a1_config_t *a,const a1_config_t *b) {
    assert(a==&cfg && b==&current);return connection;
}
static uint8_t device_language_get(void) { return 0; }
static uint32_t feature_config_revision(void) { return cfg.revision; }
static bool feature_mqtt_filters_current(void *filters,unsigned n) { assert(filters==feature_filters && n==count);return true; }
static void feature_mqtt_session(const mqtt_account_t *a) { assert(a==&account); }
static int service_mqtt_capabilities(sender_t sender,const mqtt_account_t *a) {
    assert(sender==publish_bytes && a==&account);++capabilities;
    memset(&native_publication,0,sizeof(native_publication));return MQTT_SUCCESS;
}
static int feature_mqtt_service(sender_t sender,bool connected) {
    assert(sender==publish_bytes);if(connected)++features;return MQTT_SUCCESS;
}
static void a1_filter_get(a1_filter_storage_t *filter) { filter->n=0; }
static bool a1_filter_mqtt_due(const a1_filter_storage_t *a,const a1_filter_storage_t *b,uint32_t now,uint32_t at) {
    (void)a;(void)b;(void)now;(void)at;return false;
}
static int service_mqtt_snapshot(sender_t sender,const mqtt_account_t *a) {
    assert(sender==publish_bytes && a==&account);++snapshots;
    ph_mqtt_publication_attempt(&native_publication,now_ms);
    if(!snapshot_result)ph_mqtt_publication_complete(&native_publication,now_ms);
    return snapshot_result;
}
static bool service_mqtt_status_due(bool changed) {
    return ph_mqtt_publication_due(&native_publication,now_ms,5000,1000,changed);
}
static int discovery(const a1_config_t *c,unsigned i) { assert(c==&current && i<12);return discovery_result; }
static int states(const a1_config_t *c,const mqtt_state_t *s,unsigned i) {
    assert(c==&current && s==&state && i<7);++normal;return native_result;
}
#include "production.inc"

static bool prime_metadata=true;
static void account_set(const char *user) {
 assert(mqtt_account_init(&account,1,"a1",user,"AABBCCDDEEFF"));
 /* Explicit successful retained descriptor phase; the assertions below retain
  * their original state-only cadence/IRQ scope. Reconnect re-sends metadata. */
 if(prime_metadata && !fan_sensor_metadata){
  bool handled;
  if(fan_sensor_turn)assert(!fan_sensor_cycle(true,0,&handled)&&!handled);
  assert(!fan_sensor_cycle(true,0,&handled)&&handled);
  assert(fixture_metadata);
  assert(!fan_sensor_cycle(true,0,&handled)&&!handled);
  fan_sensor_started=false;
 }
}
static void cycle(void) { assert(session_cycle(NULL)==send_result); }
static void next_feed(void) { now_ms+=5000;assert(!session_cycle(NULL));assert(!session_cycle(NULL)); }
static void set_on(bool on) { effective_on=on;cfg.fan_on=on; }

static void payload(void) {
    account_set("test");cycle();assert(sends==1 && random_calls==1 && lock_calls==1);
    set_on(false);next_feed();assert(sends==2); // effective OFF is a valid 0%, not 90% requested
    set_on(true);a1_state.fan_ready=false;next_feed();assert(sends==3);
    a1_state.fan_ready=true;a1_state.ota_active=true;next_feed();assert(sends==4);
    a1_state.ota_active=false;a1_state.reboot_pending=true;next_feed();assert(sends==5);
    a1_state.reboot_pending=false;effective_speed=100;next_feed();assert(sends==6);
    effective_speed=101;next_feed();assert(sends==7);
}
static void effective(void) {
    account_set("test");
#ifndef A1_FRESH_AIR
    // Linkage can turn the effective fan off despite an ON requested config.
    effective_on=false;cycle();effective_on=true;
#else
    cfg.fan_on=false;cycle();cfg.fan_on=true;
#endif
    next_feed();assert(sends==2 && cfg.speed==90 && effective_speed==37);
}
static void reconnect(void) {
    account_set("test");cycle();assert(sends==1);
    assert(!session_initial(NULL) && capabilities==1);
    for(unsigned i=0;i<12;i++)assert(!session_cycle(NULL));
    assert(discovery_index==12 && sends==1 && fan_sensor_sequence==1);
    account_set("next/owner");next_feed();assert(sends==2 && random_calls==1);
}
static void counters(void) {
    fan_sensor_boot=UINT32_MAX;fan_sensor_sequence=UINT32_MAX-1;account_set("test");
    cycle();next_feed();assert(sends==2 && !random_calls && fan_sensor_sequence==1);
}
static void backoff(void) {
    account_set("test");send_result=PH_MQTT_MORE;cycle();assert(attempts==1 && !sends);
    for(now_ms=0;now_ms<1000;now_ms++)assert(!session_cycle(NULL));
    send_result=0;cycle();assert(attempts==2 && sends==1 && fan_sensor_sequence==1);
    for(now_ms=1001;now_ms<6000;now_ms++)assert(!session_cycle(NULL));
    assert(attempts==2);cycle();assert(attempts==3 && sends==2);
    now_ms+=6000;send_result=-7;assert(!session_cycle(NULL));cycle();assert(attempts==4);
}
static void failures(void) {
    prime_metadata=false;account_set("test");random_failure=true;cycle();assert(!sends && random_calls==1);
    for(now_ms=0;now_ms<1000;now_ms++)assert(!session_cycle(NULL));
    random_failure=false;random_value=0;cycle();assert(!sends && random_calls==2);
    now_ms+=1000;assert(!session_cycle(NULL));random_value=UINT32_MAX;cycle();
    assert(!sends && random_calls==3 && fan_sensor_metadata);
    cycle();cycle();assert(sends==1 && fan_sensor_sequence==1);
}
static void gates(void) {
    account_set("test");wifi=false;assert(session_cycle(NULL)==PH_MQTT_RECONFIGURE && !attempts);
    wifi=true;connection=false;assert(session_cycle(NULL)==PH_MQTT_RECONFIGURE && !attempts);
    connection=true;ota_active=true;cycle();assert(!attempts);ota_active=false;
    discovery_index=0;cycle();assert(!attempts && discovery_index==1);
    discovery_result=PH_MQTT_MORE;cycle();assert(sends==1 && discovery_index==1);
}
static void fairness(void) {
    account_set("test");native_result=PH_MQTT_MORE;
    for(unsigned i=0;i<60;i++) {
        now_ms+=6000;state_pending=127;
        int rc=session_cycle(NULL);assert(rc==MQTT_SUCCESS||rc==PH_MQTT_MORE);
    }
    assert(sends==30 && normal==15 && features==30 && snapshots==15);
}
static void independent(void) {
    account_set("test");
    /* A native success cannot acknowledge any of HA's seven initial leaves. */
    fan_sensor_turn=true;filter_turn=false;state_index=state_seen=0;state_pending=127;sent=false;
    revision=cfg.revision;old_on=true;old_speed=37;old_visible=false;
    assert(!session_cycle(NULL));assert(snapshots==1&&!state_seen&&state_pending==127&&!sent);
    for(unsigned i=0;i<8;i++)assert(!session_cycle(NULL));
    assert(sent&&state_seen==127);
    /* Continuous HA/filter updates must leave native heartbeats and retries
     * progressing. A deferred native publish retains its own deadline. */
    now_ms=5000;snapshot_result=PH_MQTT_MORE;filter_turn=false;fan_sensor_turn=true;
    assert(session_cycle(NULL)==PH_MQTT_MORE);assert(snapshots==2);
    for(unsigned i=0;i<1000;i++){now_ms=5000+i;assert(!session_cycle(NULL));}
    assert(snapshots==2);now_ms=6000;snapshot_result=0;
    for(unsigned i=0;i<4;i++)assert(!session_cycle(NULL));
    assert(snapshots==3&&native_publication.sent_at==6000);
    now_ms=11000;state_pending=127;
    for(unsigned i=0;i<4;i++)assert(!session_cycle(NULL));
    assert(snapshots==4);
}
static void bounds(void) {
    char user[MQTT_USER_MAX+1];memset(user,'u',MQTT_USER_MAX);user[MQTT_USER_MAX]=0;
    account_set(user);now_ms=UINT32_MAX-2000;cycle();assert(sends==1);
    // Unsigned elapsed scheduling crosses the owner clock wrap without a burst.
    assert(!session_cycle(NULL));now_ms+=4999;assert(!session_cycle(NULL));assert(sends==1);
    now_ms++;cycle();assert(sends==2);
}
int main(int argc,char **argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"payload"))payload();
    else if(!strcmp(argv[1],"effective"))effective();
    else if(!strcmp(argv[1],"reconnect"))reconnect();
    else if(!strcmp(argv[1],"counters"))counters();
    else if(!strcmp(argv[1],"backoff"))backoff();
    else if(!strcmp(argv[1],"failures"))failures();
    else if(!strcmp(argv[1],"gates"))gates();
    else if(!strcmp(argv[1],"fairness"))fairness();
    else if(!strcmp(argv[1],"independent"))independent();
    else {assert(!strcmp(argv[1],"bounds"));bounds();}
    assert(!locked);return 0;
}

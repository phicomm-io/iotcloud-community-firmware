/* Execute the production pulse sampler, MQTT producer and owner turn. */
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "TC1/user_power.h"
#include "common/mqtt_account.h"
#include "common/mqtt/publication.h"
#include "tests/sensor_mp_fixture.h"

#define MQTT_SUCCESS 0
#define PH_MQTT_MORE 2
#define PH_MQTT_RECONFIGURE 1
#define kNoErr 0
#define INPUT_PULL_UP 0
#define IRQ_TRIGGER_FALLING_EDGE 0
#define POWER 15
#define power_log(...) ((void)0)
static struct { uint16_t power_coefficient_x100; uint32_t p_count_1_day_ago; } config={1710,0}, *user_config=&config;
static uint64_t clock_ns;
static uint32_t now_ms, random_value=UINT32_MAX;
static bool random_failure, allocation_failure, current_session=true, ota_active;
static unsigned random_calls, sends, attempts, powers, statuses,capabilities;
static int send_result,synchronous_error;
static mqtt_account_t account;
static bool power_pending,power_turn,rediscover,cleanup_pending;
static unsigned ha_refresh_pending,discovery_index=12,state_index=10,publication_cursor;
static ph_mqtt_publication native_publication;
static void (*power_irq)(void *);
static uint64_t mico_nanosecond_clock_value(void) { return clock_ns; }
static uint32_t mico_rtos_get_time(void) { return now_ms; }
static void MicoGpioInitialize(unsigned pin,unsigned mode) { assert(pin==POWER && mode==INPUT_PULL_UP); }
static void MicoGpioEnableIRQ(unsigned pin,unsigned mode,void (*handler)(void *),void *arg) {
    assert(pin==POWER && mode==IRQ_TRIGGER_FALLING_EDGE && !arg);power_irq=handler;
}
static int MicoRandomNumberRead(uint8_t *out,size_t n) {
    assert(n==sizeof(random_value));++random_calls;
    if(random_failure)return -1;
    memcpy(out,&random_value,n);return kNoErr;
}
static int service_sender(const char *topic,const void *body,size_t n,bool retained) {
    assert(strlen(topic)<192 && n<512);
    if(retained){assert(fixture_sensor_output(topic,body,n,true,"power","Power","W",0));return 0;}
    ++attempts;fixture_sensor_output(topic,body,n,false,"power","Power","W",send_result);
    if(send_result)return send_result;
    ++sends;return 0;
}
static bool session_current(void) { return current_session; }
static int ota_service(const char *base) { assert(base==account.base);return MQTT_SUCCESS; }
static bool ota_job_active(void) { return ota_active; }
static int snapshot_step(void) {
    ++powers;
    if(discovery_index<12)++discovery_index;else state_index=10;
    return MQTT_SUCCESS;
}
static int service_mqtt_snapshot(int (*sender)(const char *,const void *,size_t,bool),const mqtt_account_t *a) {
    assert(sender==service_sender && a==&account);++statuses;
    ph_mqtt_publication_attempt(&native_publication,now_ms);ph_mqtt_publication_complete(&native_publication,now_ms);return MQTT_SUCCESS;
}
static bool service_mqtt_status_due(bool changed){return ph_mqtt_publication_due(&native_publication,now_ms,5000,1000,changed);}
static int service_mqtt_capabilities(int (*sender)(const char *,const void *,size_t,bool),const mqtt_account_t *a) {
    assert(sender==service_sender && a==&account);++capabilities;
    memset(&native_publication,0,sizeof(native_publication));return MQTT_SUCCESS;
}
void *ph_wire_malloc(size_t n){return allocation_failure?NULL:malloc(n);}
#include "production.inc"

static void pulse(uint64_t at) { clock_ns=at;now_ms=(uint32_t)(at/1000000);assert(power_irq);power_irq(NULL); }
static void sample(void) { PowerInit();pulse(100000000);pulse(1200000000);clock_ns=1300000000;now_ms=1300; }
static bool prime_metadata=true;
static void account_set(const char *user) {
 assert(mqtt_account_init(&account,1,"tc1",user,"AABBCCDDEEFF"));
 /* Explicit successful retained descriptor phase; the assertions below retain
  * their original state-only cadence/IRQ scope. Reconnect re-sends metadata. */
 if(prime_metadata && !power_sensor_metadata){
  bool handled;
  if(power_sensor_turn)assert(!power_sensor_cycle(&handled)&&!handled);
  assert(!power_sensor_cycle(&handled)&&handled);
  assert(fixture_metadata);
  assert(!power_sensor_cycle(&handled)&&!handled);
  power_sensor_started=false;
 }
}
static void cycle(void) { assert(session_cycle(NULL)==send_result); }
static void feed_at(uint32_t at) { now_ms=at;assert(!session_cycle(NULL));assert(!session_cycle(NULL)); }
static void next_feed(void) { feed_at(now_ms+5000); }

static void reader(void) {
    tc1_power_sample_t s;PowerInit();PowerSampleRead(&s);assert(!s.valid && !s.age_ms);
    pulse(100000000);PowerSampleRead(&s);assert(!s.valid); // no calculated result yet
    pulse(1200000000);clock_ns=1300000000;now_ms=1300;PowerSampleRead(&s);
    assert(s.valid && s.age_ms==100 && fabs(s.watts-3.1090909)<0.00001);
    now_ms=21199;PowerSampleRead(&s);assert(s.valid && s.age_ms==19999);
    now_ms=21200;PowerSampleRead(&s);assert(!s.valid && s.age_ms==20000 && s.watts>0);
    now_ms=1300;PowerSampleRead(&s);assert(!s.valid && s.age_ms==UINT32_MAX); // no revival after a full tick wrap
    // A genuinely calculated zero is valid; initialization alone was not.
    power_sample_commit(0,now_ms);PowerSampleRead(&s);assert(s.valid && s.watts==0 && !s.age_ms);
    power_sample_commit(NAN,now_ms);PowerSampleRead(&s);assert(!s.valid);
    power_sample_commit(-10,now_ms);PowerSampleRead(&s);assert(!s.valid);
    power_sample_commit(INFINITY,now_ms);PowerSampleRead(&s);assert(!s.valid);
    now_ms=UINT32_MAX-500;power_sample_commit(125,now_ms);
    now_ms+=1234;PowerSampleRead(&s);assert(s.valid && s.age_ms==1234 && s.watts==12.5);
    PowerSampleInvalidate();PowerSampleRead(&s);assert(!s.valid && s.age_ms==UINT32_MAX);
    power_sample_commit(125,now_ms);PowerSampleRead(&s);assert(s.valid && !s.age_ms);
    __atomic_store_n(&power_sample_version,1,__ATOMIC_SEQ_CST);
    PowerSampleRead(&s);assert(!s.valid); // bounded reader never spins on an interrupted writer
}
static void *writer(void *arg) {
    (void)arg;
    for(unsigned i=0;i<20000;i++)power_sample_commit(i%2?100:200,i%2?1000:2000);
    return NULL;
}
static void concurrent(void) {
    now_ms=3000;power_sample_commit(100,1000);
    pthread_t thread;assert(!pthread_create(&thread,NULL,writer,NULL));
    for(unsigned i=0;i<20000;i++) {
        tc1_power_sample_t s;PowerSampleRead(&s);
        if(s.valid)assert((s.watts==10 && s.age_ms==2000)||(s.watts==20 && s.age_ms==1000));
    }
    assert(!pthread_join(thread,NULL));
}
static void payload(void) {
    account_set("test");sample();cycle();assert(sends==1 && random_calls==1);
    next_feed();assert(sends==2 && random_calls==1);
    feed_at(21200);assert(sends==3); // stale publishes null immediately
    // Reconfiguration does not reset the process boot or publication sequence.
    account_set("next/owner");assert(!session_initial(NULL) && capabilities==1);
    for(unsigned i=0;i<13;i++)assert(!session_cycle(NULL));
    assert(discovery_index==12 && state_index==10 && sends==3 && power_sensor_sequence==3);
    account_set("next/owner");next_feed();assert(sends==4 && random_calls==1);
    power_sample_commit(125,now_ms+5000);next_feed();assert(sends==5 && random_calls==1);
}
static void counters(void) {
    power_sensor_boot=UINT32_MAX;power_sensor_sequence=UINT32_MAX-1;account_set("test");sample();
    cycle();next_feed();assert(sends==2 && !random_calls && power_sensor_sequence==1);
}
static void backoff(void) {
    account_set("test");sample();send_result=PH_MQTT_MORE;cycle();assert(attempts==1 && !sends);
    uint32_t start=now_ms;for(now_ms=start;now_ms-start<1000;now_ms++)assert(!session_cycle(NULL));
    send_result=0;cycle();assert(attempts==2 && sends==1 && power_sensor_sequence==1);
    for(now_ms=start+1001;now_ms-start<6000;now_ms++)assert(!session_cycle(NULL));
    assert(attempts==2);cycle();assert(attempts==3 && sends==2);
    now_ms+=6000;send_result=-7;assert(!session_cycle(NULL));cycle();assert(attempts==4);
}
static void failures(void) {
    prime_metadata=false;account_set("test");sample();random_failure=true;cycle();assert(!sends && random_calls==1);
    uint32_t start=now_ms;for(now_ms=start;now_ms-start<1000;now_ms++)assert(!session_cycle(NULL));
    random_failure=false;random_value=0;cycle();assert(!sends && random_calls==2);
    now_ms+=1000;assert(!session_cycle(NULL));random_value=UINT32_MAX;allocation_failure=true;cycle();assert(!sends && random_calls==3);
    now_ms+=1000;assert(!session_cycle(NULL));allocation_failure=false;cycle();assert(!sends && random_calls==3 && power_sensor_metadata);
    cycle();cycle();assert(sends==1 && random_calls==3);
}
static void gates(void) {
    account_set("test");sample();current_session=false;
    assert(session_cycle(NULL)==PH_MQTT_RECONFIGURE && !attempts);current_session=true;
    ota_active=true;cycle();assert(!attempts);ota_active=false;
    discovery_index=0;cycle();assert(!attempts);discovery_index=12;state_index=0;
    cycle();assert(!attempts);cycle();assert(sends==1);
    account.mode=0;now_ms+=5000;cycle();cycle();assert(sends==1);
}
static void fairness(void) {
    account_set("test");sample();
    for(unsigned i=0;i<60;i++) {
        now_ms+=6000;power_pending=true;cycle();
    }
    assert(sends==30 && statuses==15 && powers==15);
}
static void bounds(void) {
    char user[MQTT_USER_MAX+1];memset(user,'u',MQTT_USER_MAX);user[MQTT_USER_MAX]=0;
    account_set(user);cycle();assert(sends==1); // initial zero remains invalid, maximum account key fits
    now_ms+=5000;power_sample_commit(0,now_ms);feed_at(now_ms);assert(sends==2);
}
int main(int argc,char **argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"reader"))reader();
    else if(!strcmp(argv[1],"concurrent"))concurrent();
    else if(!strcmp(argv[1],"payload"))payload();
    else if(!strcmp(argv[1],"counters"))counters();
    else if(!strcmp(argv[1],"backoff"))backoff();
    else if(!strcmp(argv[1],"failures"))failures();
    else if(!strcmp(argv[1],"gates"))gates();
    else if(!strcmp(argv[1],"fairness"))fairness();
    else {assert(!strcmp(argv[1],"bounds"));bounds();}
    return 0;
}

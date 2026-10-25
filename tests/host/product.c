/* Execute the unmodified firmware MQTT worker with a POSIX transport and fake
 * peripheral/configuration APIs. This is protocol evidence, not hardware QA. */
#include "mico.h"
#include "mico_system.h"
#include "../../common/phicomm_features.h"
extern uint64_t host_now(void);
static uint64_t started;
static unsigned duration=10;
static feature_config_t features;
static unsigned controls;
uint8_t device_language_get(void){return features.language==1?1:0;}
/* This harness exercises the original broker; Bemfa has its own dual-owner
 * protocol fixture. Keep the original broker enabled in these scenarios. */
bool bemfa_primary_enabled(void){return true;}
static void (*scheduled_worker)(mico_thread_arg_t);
static mico_thread_arg_t scheduled_argument;
int mico_rtos_create_thread(void *output,int priority,const char *name,void (*fn)(mico_thread_arg_t),unsigned stack,int argument){
 (void)priority;(void)name;(void)stack;
 assert(output&&!scheduled_worker);*(mico_thread_t *)output=1;
 scheduled_worker=fn;scheduled_argument=argument;return kNoErr;
}
uint32_t feature_config_revision(void){return features.revision;}
void feature_config_get(feature_config_t *out){*out=features;feature_config_limit_peers(out);}
bool feature_fan_auto(void){return features.linkage.enabled;}
unsigned feature_fan_limit(void){return features.linkage.max_speed;}
int feature_fan_command(json_object *o){(void)o;return 1;}
bool feature_bound(void){return false;}
#ifdef HOST_A1
bool feature_ha_device(char *id,size_t ni,char *name,size_t nn,bool *grouped){
 (void)id;(void)ni;(void)name;(void)nn;(void)grouped;return false;
}
#endif
unsigned feature_mqtt_filters(char f[][FEATURE_TOPIC_MAX],unsigned capacity){(void)capacity;(void)f;return 0;}
bool feature_mqtt_input(const char *t,const void *d,size_t n,bool retained){(void)t;(void)d;(void)n;(void)retained;return false;}
bool feature_mqtt_filters_current(char f[][FEATURE_TOPIC_MAX], unsigned n){(void)f;(void)n;return true;}
void feature_mqtt_session(const mqtt_account_t *a){(void)a;}
bool feature_mqtt_initial_ready(void){return true;}
int feature_mqtt_service(int (*p)(const char *,const char *,bool),bool connected){(void)p;(void)connected;return 0;}
int feature_mqtt_cleanup(int (*p)(const char *,const char *,bool)){(void)p;return 0;}
void feature_effective_fan(bool a,unsigned b,bool *on,unsigned *speed){*on=a;*speed=b;}
uint32_t mico_rtos_get_time(void){return host_now()-started;}
static void trace_status(void);
static void host_update(void);
void mico_thread_msleep(unsigned ms){host_update();trace_status();if(mico_rtos_get_time()>duration*1000){printf("DONE controls=%u\n",controls);fflush(stdout);exit(0);}usleep(ms*1000);}
void mico_rtos_thread_sleep(unsigned sec){mico_thread_msleep(sec*1000);}
#ifdef HOST_M1
#include "../../M1/mqtt.c"
/* Network protocol harness never launches a firmware download. */
bool m1_ota_download(const char *url){(void)url;return false;}
static m1_config_t config;
char m1_id[13]="aabbccddeeff";mico_mutex_t m1_lock;m1_state_t m1_state;
void m1_config_get(m1_config_t *out){*out=config;}
void m1_state_get(m1_state_t *out){
    *out=m1_state;
}
json_object *m1_particles_json(void){return json_object_new_object();}
bool m1_set_brightness(unsigned n){++controls;m1_state.brightness_requested=n;printf("CONTROL brightness=%u\n",n);fflush(stdout);return true;}
bool m1_sample_fresh(const m1_sample_t *s,uint32_t now){return s->received && (uint32_t)(now-s->updated_ms)<15000;}
#define WORKER mqtt_worker
#elif defined(HOST_A1)
#include "../../A1/mqtt.c"
bool a1_ota_download(const char *url){(void)url;return false;}
static a1_config_t config;
char a1_id[13]="aabbccddeeff";mico_mutex_t a1_lock;a1_state_t a1_state;
void a1_filter_get(a1_filter_storage_t *out){a1_filter_defaults(out);}
void a1_filter_stock_get(a1_filter_stock_t *out){a1_filter_stock_defaults(out,false);}
void a1_config_get(a1_config_t *out){*out=config;}
void a1_state_get(a1_state_t *out){*out=a1_state;}
bool a1_control_local(const char *kind,json_object *o){++controls;printf("CONTROL %s\n",kind);fflush(stdout);if(!strcmp(kind,"fan")){json_object *v=json_object_object_get(o,"on");if(v)config.fan_on=json_object_get_boolean(v);v=json_object_object_get(o,"speed");if(v)config.speed=json_object_get_int(v);}++config.revision;return true;}
bool a1_control_apply(a1_control_t *r,bool button,bool broadcast){
 (void)button;(void)broadcast;++controls;
 printf("CONTROL %s\n",r->kind==A1_CONTROL_FAN?"fan":r->kind==A1_CONTROL_LIGHT?"light":"child_lock");fflush(stdout);
 if(r->kind==A1_CONTROL_FAN){if(r->fields&A1_SET_ON)config.fan_on=r->on;if(r->fields&A1_SET_SPEED)config.speed=r->speed;}
 ++config.revision;return true;
}
json_object *a1_json_parse(const char *s,size_t n){(void)n;return json_tokener_parse(s);}
#define WORKER mqtt_worker
#else
#define WEB_LOG_H
#define web_log(...) ((void)0)
#include "../../TC1/mqtt_server/user_mqtt_client.c"
/* This harness has no concurrent SDK configuration restore transaction. The
 * real production guard is linked and initialized, rather than stubbing locks. */
bool tc1_config_changing(void){return false;}
OSStatus ota_server_start(char *url,char *hash,ota_server_cb_fn callback){(void)url;(void)hash;(void)callback;return -1;}
void tc1_ota_snapshot(char *state,char *error,unsigned *progress){strcpy(state,"idle");error[0]=0;*progress=0;}
static user_config_t config;
static system_config_t system_config;
user_config_t *user_config=&config;system_config_t *sys_config=&system_config;
char str_mac[16]="AABBCCDDEEFF";int childLockEnabled;
volatile uint32_t power_session_pulses=100;
uint32_t p_count=100;float real_time_power=12.5;
void UserLedSet(char n){(void)n;}
void UserRelaySet(unsigned char i,char on){if(i>=6)return;++controls;config.socket_status[i]=on;printf("CONTROL socket=%u on=%d\n",i,on);fflush(stdout);}
void UserRelaySetAll(char on){for(unsigned i=0;i<6;++i)UserRelaySet(i,on);}
bool RelayOut(void){for(unsigned i=0;i<6;++i)if(config.socket_status[i])return true;return false;}
void UserSoftReboot(void){}
float PowerPulseCountToKwh(uint32_t count){return count/100.0;}
void PowerSampleRead(tc1_power_sample_t *out){memset(out,0,sizeof(*out));}
void PowerSampleInvalidate(void){}
#define WORKER MqttClientThread
#endif
#include "service_adapter.inc"
static void host_update(void) {
#ifdef HOST_M1
    /* Simulated UART production is independent of any state getter. */
    if(mico_rtos_get_time()>=2000 && mico_rtos_get_time()<7000){m1_state.sample.received=true;m1_state.sample.sequence=1;m1_state.sample.updated_ms=2000;}
#endif
    static bool switched;
    const char *user=getenv("HOST_SWITCH_USER");
    if(user && !switched && mico_rtos_get_time()>=4000) {
        switched=true; strcpy(config.mqtt_user,user);
        printf("CONFIG account switch\n"); fflush(stdout);
    }
}
static void trace_status(void) {
    static int last=-1;
#ifdef HOST_M1
    unsigned status=m1_state.mqtt_status;
#elif defined(HOST_A1)
    unsigned status=a1_state.mqtt_status;
#else
    unsigned status=tc1_mqtt_status;
#endif
    if(last!=(int)status){printf("STATUS %s\n",mqtt_status_name(status));fflush(stdout);last=status;}
}
int main(int argc,char **argv){
    if(argc<4){fprintf(stderr,"host-worker host port user [duration]\n");return 2;}
    service_init();started=host_now();if(argc>4)duration=atoi(argv[4]);
    const char *pw=getenv("HOST_MQTT_PASSWORD");if(!pw)pw="test-password";
#ifdef HOST_M1
    strcpy(config.mqtt_host,argv[1]);config.mqtt_port=atoi(argv[2]);strcpy(config.mqtt_user,argv[3]);strcpy(config.mqtt_password,pw);strcpy(config.name,"M1 test");config.mqtt_topic_mode=1;m1_state.wifi_connected=true;
#elif defined(HOST_A1)
    strcpy(config.mqtt_host,argv[1]);config.mqtt_port=atoi(argv[2]);strcpy(config.mqtt_user,argv[3]);strcpy(config.mqtt_password,pw);strcpy(config.name,"A1 test");config.mqtt_topic_mode=1;a1_state.wifi_connected=true;config.speed=60;
#else
    if(!tc1_status_guard_init())return 3;
    strcpy(config.mqtt_host,argv[1]);config.mqtt_port=atoi(argv[2]);strcpy(config.mqtt_user,argv[3]);strcpy(config.mqtt_password,pw);strcpy(system_config.micoSystemConfig.name,"TC1 \"quoted\" test");config.mqtt_topic_mode=getenv("TC1_TEST_LEGACY")?0:1;
    for(unsigned i=0;i<6;++i)snprintf(config.socket_names[i],64,"Socket \"%u\"",i);
#endif
#ifdef HOST_M1
    if(m1_mqtt_start())return 3;
#elif defined(HOST_A1)
    if(a1_mqtt_start())return 3;
#else
    if(UserMqttInit())return 3;
#endif
    assert(scheduled_worker==WORKER);
    scheduled_worker(scheduled_argument);return 0;
}

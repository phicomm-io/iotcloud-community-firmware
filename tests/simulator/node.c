/* Production service, automation, binding worker and MQTT worker run unchanged.
 * Only RTOS, Flash and physical peripherals are replaced by POSIX/RAM shims. */
#include "mico.h"
#include "../../common/watchdog.h"
/* This POSIX simulator has no hardware watchdog; deadline behavior is covered
 * by the production monitor tests in test_watchdog.c. */
void watchdog_beat(watchdog_slot_t slot) { (void)slot; }
#include "../../common/automation.h"
#ifdef HOST_A1
#include "../../A1/a1.h"
#define CFG a1_config_t
#define STATE a1_state_t
#define config_get a1_config_get
#define config_save a1_config_save
#define state_get a1_state_get
#define status_json a1_status_json
#define device_id a1_id
#define device_lock a1_lock
#define device_state a1_state
#define mqtt_start a1_mqtt_start
#else
#include "../../M1/m1.h"
#define CFG m1_config_t
#define STATE m1_state_t
#define config_get m1_config_get
#define config_save m1_config_save
#define state_get m1_state_get
#define status_json m1_status_json
#define device_id m1_id
#define device_lock m1_lock
#define device_state m1_state
#define mqtt_start m1_mqtt_start
#endif
extern uint64_t host_now(void);
static uint64_t started;
char device_id[13];
mico_mutex_t device_lock;
STATE device_state;
#ifdef HOST_A1
mico_mutex_t a1_config_lock;
#define BASE_LOCK() mico_rtos_lock_mutex(&a1_config_lock)
#define BASE_UNLOCK() mico_rtos_unlock_mutex(&a1_config_lock)
#else
#define BASE_LOCK() pthread_mutex_lock(&store_lock)
#define BASE_UNLOCK() pthread_mutex_unlock(&store_lock)
#endif
static CFG base_config;
static feature_config_t features;
static automation_config_t automations;
static pthread_mutex_t store_lock=PTHREAD_MUTEX_INITIALIZER;
static bool sensor_enabled=true;
static float sensor_values[4]={22,45,50,.05};
uint32_t mico_rtos_get_time(void){return host_now()-started;}
void mico_thread_msleep(unsigned n){usleep(n*1000);}
void mico_rtos_thread_sleep(unsigned n){sleep(n);}
struct launch {void (*fn)(mico_thread_arg_t);int arg;};
static void *launch_thread(void *p){struct launch l=*(struct launch *)p;free(p);l.fn(l.arg);return NULL;}
int mico_rtos_create_thread(void *out,int priority,const char *name,void (*fn)(mico_thread_arg_t),unsigned stack,int arg){
 (void)priority;(void)name;(void)stack;struct launch *l=malloc(sizeof(*l));if(!l)return -1;*l=(struct launch){fn,arg};pthread_t t;int rc=pthread_create(&t,NULL,launch_thread,l);if(rc){free(l);return rc;}if(out)*(pthread_t *)out=t;pthread_detach(t);return 0;
}
#undef bind
int simulator_bind(int fd,const struct sockaddr *a,socklen_t n){struct sockaddr_in copy=*(const struct sockaddr_in *)a;const char *ip=getenv("SIM_BIND_IP");if(ip && copy.sin_addr.s_addr==INADDR_ANY)inet_aton(ip,&copy.sin_addr);return bind(fd,(struct sockaddr *)&copy,n);}
void config_get(CFG *c){BASE_LOCK();*c=base_config;BASE_UNLOCK();}
bool config_save(const CFG *c){BASE_LOCK();bool ok=c->revision==base_config.revision;if(ok){base_config=*c;++base_config.revision;}BASE_UNLOCK();return ok;}
void feature_config_get(feature_config_t *c){pthread_mutex_lock(&store_lock);*c=features;pthread_mutex_unlock(&store_lock);}
bool feature_config_save(const feature_config_t *c){pthread_mutex_lock(&store_lock);bool ok=c->revision==features.revision;if(ok){features=*c;++features.revision;}pthread_mutex_unlock(&store_lock);return ok;}
void automation_load(automation_config_t *c){pthread_mutex_lock(&store_lock);*c=automations;pthread_mutex_unlock(&store_lock);}
bool automation_save(const automation_config_t *c){pthread_mutex_lock(&store_lock);bool ok=c->revision==automations.revision;if(ok){automations=*c;++automations.revision;}pthread_mutex_unlock(&store_lock);return ok;}
void state_get(STATE *s){mico_rtos_lock_mutex(&device_lock);
#ifndef HOST_A1
 if(sensor_enabled){device_state.sample.received=true;device_state.sample.updated_ms=mico_rtos_get_time();device_state.sample.sequence=device_state.sample.updated_ms/1000+1;device_state.sample.temperature_deci_c=sensor_values[0]*10;device_state.sample.humidity_deci_pct=sensor_values[1]*10;device_state.sample.pm25_ug_m3=sensor_values[2];device_state.sample.hcho_ug_m3=sensor_values[3]*1000;}
#endif
 *s=device_state;mico_rtos_unlock_mutex(&device_lock);}
json_object *status_json(void){CFG c;STATE s;config_get(&c);state_get(&s);json_object *o=json_object_new_object();json_object_object_add(o,"id",json_object_new_string(device_id));json_object_object_add(o,"mqtt_connected",json_object_new_boolean(s.mqtt_connected));json_object_object_add(o,"mqtt_errors",json_object_new_int(s.mqtt_errors));json_object_object_add(o,"uptime_ms",json_object_new_int(mico_rtos_get_time()));
#ifdef HOST_A1
 json_object_object_add(o,"filter",a1_filter_json());
 json_object_object_add(o,"coordinator",json_object_new_boolean(feature_is_coordinator()));bool on;unsigned speed;feature_effective_fan(c.fan_on,c.speed,&on,&speed);json_object_object_add(o,"on",json_object_new_boolean(on));json_object_object_add(o,"speed",json_object_new_int(speed));
#else
 json_object_object_add(o,"brightness",json_object_new_int(s.brightness_requested));
#endif
 return o;}
#ifdef HOST_A1
static a1_filter_storage_t sim_filter={A1_FILTER_MAGIC,0,A1_FILTER_DEFAULT_HOURS};
static a1_filter_clock_t sim_filter_clock;
void a1_filter_tick(bool running){BASE_LOCK();a1_filter_advance(&sim_filter,&sim_filter_clock,mico_rtos_get_time(),running);BASE_UNLOCK();}
void a1_filter_get(a1_filter_storage_t *out){CFG c;config_get(&c);bool on;unsigned speed;feature_effective_fan(c.fan_on,c.speed,&on,&speed);a1_filter_tick(on&&speed);BASE_LOCK();*out=sim_filter;BASE_UNLOCK();}
const char *a1_filter_change(unsigned hours,bool reset){a1_filter_storage_t f;a1_filter_get(&f);BASE_LOCK();if(reset){sim_filter.used_seconds=0;sim_filter_clock.remainder_ms=0;}else sim_filter.lifetime_hours=hours;BASE_UNLOCK();return "accepted";}
a1_config_t *a1_config=&base_config;
void a1_control_changed(void){++base_config.revision;}
bool a1_config_flush(bool f){(void)f;return true;}
bool a1_reset(bool f){(void)f;return false;}
bool a1_reboot_later(uint32_t n){(void)n;return false;}
bool a1_scan_start(void){return false;}
bool a1_wifi_trial(const CFG *c){(void)c;return false;}
bool a1_ota_download(const char *u){(void)u;return false;}
#else
bool m1_set_brightness(unsigned n){mico_rtos_lock_mutex(&device_lock);device_state.brightness_requested=n;mico_rtos_unlock_mutex(&device_lock);return true;}
bool m1_sample_fresh(const m1_sample_t *s,uint32_t now){return s->received&&(uint32_t)(now-s->updated_ms)<15000;}
bool m1_reset(bool f){(void)f;return false;}
bool m1_reboot_later(uint32_t n){(void)n;return false;}
bool m1_scan_start(void){return false;}
bool m1_wifi_trial(const CFG *c){(void)c;return false;}
bool m1_ota_download(const char *u){(void)u;return false;}
#endif
int main(int argc,char **argv){if(argc!=6){fprintf(stderr,"node id ip broker port user\n");return 2;}started=host_now();snprintf(device_id,sizeof(device_id),"%s",argv[1]);mico_rtos_init_mutex(&device_lock);
#ifdef HOST_A1
 mico_rtos_init_mutex(&a1_config_lock);base_config.speed=60;base_config.brightness=255;base_config.led_enabled=1;
#endif
 snprintf(device_state.ip,sizeof(device_state.ip),"%s",argv[2]);device_state.wifi_connected=true;snprintf(base_config.mqtt_host,sizeof(base_config.mqtt_host),"%s",argv[3]);base_config.mqtt_port=atoi(argv[4]);snprintf(base_config.mqtt_user,sizeof(base_config.mqtt_user),"%s",argv[5]);strcpy(base_config.name,"Runtime simulator");base_config.mqtt_topic_mode=1;feature_defaults(&features);automation_defaults(&automations);service_init();if(feature_start()||mqtt_start())return 3;
 char *line=NULL;size_t capacity=0;while(getline(&line,&capacity,stdin)>0){json_object *q=service_parse(line,strlen(line));if(!q){puts("{}");fflush(stdout);continue;}json_object *meta=json_object_object_get(q,"sim_sensor");if(meta){mico_rtos_lock_mutex(&device_lock);json_object *v=json_object_object_get(meta,"enabled");if(v)sensor_enabled=json_object_get_boolean(v);v=json_object_object_get(meta,"values");if(v)for(unsigned i=0;i<4;i++)sensor_values[i]=json_object_get_double(json_object_array_get_idx(v,i));mico_rtos_unlock_mutex(&device_lock);puts("{\"code\":\"ok\"}");}else{json_object *r=service_command(q);puts(json_object_to_json_string(r));json_object_put(r);}json_object_put(q);fflush(stdout);}free(line);return 0;}

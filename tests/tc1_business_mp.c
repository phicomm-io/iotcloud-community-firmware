/* Exercise the actual TC1 adapter with deterministic hardware/storage seams. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/typed_wire.h"
#include "common/i18n.h"
#define maxNameLen 64
#define SOCKET_NAME_LENGTH 64
#define ZTC1_NAME "TC1_A1-%s"
#define POWER_COEFFICIENT_MIN_X100 100
#define POWER_COEFFICIENT_MAX_X100 10000
#define MICO_APPLICATION_PRIORITY 1
#define kNoErr 0
typedef int mico_thread_arg_t;
typedef struct {bool mqtt_enabled;unsigned revision;uint8_t language;} bemfa_config_t;
typedef struct {char mqtt_host[128],mqtt_user[64],mqtt_password[257],web_password[33],socket_names[6][64];int mqtt_port,mqtt_report_freq;uint8_t mqtt_topic_mode,user[64],socket_status[6];bool power_led_enabled;int power_coefficient_x100;struct{int timezone;} automation;bemfa_config_t bemfa;} user_config_t;
static user_config_t config,*user_config=&config;
static struct {struct {char name[64],ssid[33],user_key[65],key[65];unsigned user_keyLength,keyLength;}micoSystemConfig;} system_data,*sys_config=&system_data;
static char str_mac[16]="AABBCCDDEEFF";
static bool reserved,deny_reserve,fail_storage,recovery;
static unsigned writes,relay_writes,refreshes,reboots;
static int childLockEnabled;
static float real_time_power=125;
static unsigned p_count;
static bool tc1_config_reserve(void){if(deny_reserve||reserved)return false;reserved=true;return true;}
static void tc1_config_release(void){assert(reserved);reserved=false;}
static void tc1_status_lock(void){}
static void tc1_status_unlock(void){}
static bool device_config_recovery(void){return recovery;}
static void device_config_recovery_set(bool b){recovery=b;}
static int mico_system_context_update(void *p){assert(p==sys_config);writes++;return fail_storage?-1:0;}
static int mico_system_context_restore(void *p){assert(p==sys_config);return fail_storage?-1:0;}
static int mico_rtos_create_thread(void *p,int priority,const char *name,void(*fn)(int),unsigned stack,int arg){(void)p;(void)priority;(void)name;(void)fn;(void)stack;(void)arg;reboots++;return 0;}
static void reboot_worker(int arg){(void)arg;}
static bool RelayOut(void){for(unsigned i=0;i<6;i++)if(config.socket_status[i])return true;return false;}
static void UserLedSet(bool enabled){(void)enabled;}
static void UserRelaySet(unsigned i,bool on){assert(i<6);config.socket_status[i]=on;relay_writes++;}
static void UserRelaySetAll(bool on){for(unsigned i=0;i<6;i++)UserRelaySet(i,on);}
static void UserMqttRefreshState(bool discovery){(void)discovery;refreshes++;}
static bool tc1_auth_password_save(const char *p){strcpy(config.web_password,p);return true;}
static bool tc1_auth_reset(bool full){(void)full;return true;}
static char owned_ssid[33],owned_password[65],owned_url[512];
static bool WifiConnect(char *ssid,char *password){strcpy(owned_ssid,ssid);strcpy(owned_password,password);return true;}
static int WifiScanStart(void){return 0;}
static bool WifiScanMp(ph_writer *w){phw_begin(w,NULL,false);phw_bool(w,"scanning",false);phw_begin(w,"networks",true);phw_end(w);phw_end(w);return true;}
static int ota_server_start(char *url,void *md5,void *cb){(void)md5;(void)cb;strcpy(owned_url,url);return 0;}
static float PowerPulseCountToKwh(unsigned n){return n/1000.0f;}
static void bemfa_config_get(bemfa_config_t *next){*next=config.bemfa;}
static bool bemfa_config_values_mp(bemfa_config_t *next,const ph_value *p,bool require){(void)next;(void)p;(void)require;return false;}
static void bemfa_config_mp_fields(ph_writer *w){phw_bool(w,"mqtt_enabled",config.bemfa.mqtt_enabled);}
#include "TC1/service_mp.inc"
static void check(ph_writer *q,const char *action,const char *expected){
 phw_end(q);ph_buf *input=phw_take(q);assert(input);ph_value params;assert(phv_parse(1,input->buf,input->len,&params));
 ph_writer out;assert(phw_init(&out,1));const char *code="unset";
 assert(service_product_mp(action,&params,&out,&code));assert(!strcmp(code,expected));
 memset(input->buf,0,input->len);ph_buf_free(input);ph_buf *wire=phw_take(&out);assert(wire);ph_value payload;assert(phv_parse(1,wire->buf,wire->len,&payload));ph_buf_free(wire);
}
static void start(ph_writer *w){assert(phw_init(w,1));phw_begin(w,NULL,false);}
int main(void){
 ph_writer q;
 const char *reads[]={"config.get","power.get","time.get","wifi.scan.get",NULL};
 for(unsigned i=0;reads[i];i++){start(&q);check(&q,reads[i],"ok");start(&q);phw_int(&q,"unknown",1);check(&q,reads[i],"invalid_params");}
 for(int i=0;i<6;i++){start(&q);phw_int(&q,"index",i);phw_bool(&q,"on",true);check(&q,"outlet.set","accepted");assert(config.socket_status[i]);}
 unsigned oldwrites=writes;start(&q);phw_int(&q,"index",6);phw_bool(&q,"on",false);check(&q,"outlet.set","invalid_params");assert(writes==oldwrites);
 start(&q);phw_int(&q,"on",1);check(&q,"outlet.set","invalid_params");assert(writes==oldwrites);
 start(&q);phw_bool(&q,"on",false);check(&q,"outlet.set","accepted");assert(!RelayOut());
 fail_storage=true;start(&q);phw_bool(&q,"on",true);check(&q,"outlet.set","storage_error");assert(!RelayOut());fail_storage=false;
 deny_reserve=true;oldwrites=writes;start(&q);phw_bool(&q,"on",true);check(&q,"outlet.set","busy");assert(!RelayOut()&&writes==oldwrites);deny_reserve=false;
 const char *flags[]={"indicator.set","child_lock.set",NULL};for(unsigned i=0;flags[i];i++){start(&q);phw_bool(&q,"enabled",true);check(&q,flags[i],"accepted");}
 start(&q);phw_int(&q,"timezone_minutes",-720);check(&q,"time.set","accepted");assert(config.automation.timezone==-720);
 start(&q);phw_int(&q,"timezone_minutes",841);check(&q,"time.set","invalid_params");
 start(&q);phw_text(&q,"section","auth");phw_begin(&q,"values",false);phw_text(&q,"password","adminfjr");phw_end(&q);check(&q,"config.set","accepted");assert(!strcmp(config.web_password,"adminfjr"));
 start(&q);phw_text(&q,"section","wifi");phw_begin(&q,"values",false);phw_text(&q,"ssid","borrowed");phw_text(&q,"password","password");phw_end(&q);check(&q,"config.set","accepted");assert(!strcmp(owned_ssid,"borrowed")&&!strcmp(owned_password,"password"));
 start(&q);phw_text(&q,"url","http://fixture/firmware.bin");check(&q,"ota.start","accepted");assert(!strcmp(owned_url,"http://fixture/firmware.bin"));
 start(&q);check(&q,"wifi.scan","accepted");assert(!reserved);
 assert(refreshes&&relay_writes);
 return 0;
}

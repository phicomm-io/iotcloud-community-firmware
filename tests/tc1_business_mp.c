/* Exercise the actual TC1 adapter with deterministic hardware/storage seams. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/typed_wire.h"
#include "common/i18n.h"
#include "common/mqtt/bemfa_config.c"
#define maxNameLen 64
#define SOCKET_NAME_LENGTH 64
#define SOCKET_NUM 6
#define ZTC1_NAME "TC1_A1-%s"
#define POWER_COEFFICIENT_MIN_X100 100
#define POWER_COEFFICIENT_MAX_X100 10000
#define MICO_APPLICATION_PRIORITY 1
#define kNoErr 0
typedef int mico_thread_arg_t;
typedef struct {char mqtt_host[128],mqtt_user[64],mqtt_password[257],web_password[33],socket_names[6][64];int mqtt_port,mqtt_report_freq;uint8_t mqtt_topic_mode,user[64],socket_status[6];bool power_led_enabled;int power_coefficient_x100;struct{int timezone;} automation;bemfa_config_t bemfa;} user_config_t;
static user_config_t config,*user_config=&config;
static struct {struct {char name[64],ssid[33],user_key[65],key[65];unsigned user_keyLength,keyLength;}micoSystemConfig;} system_data,*sys_config=&system_data;
static char str_mac[16]="AABBCCDDEEFF";
static bool reserved,deny_reserve,fail_storage,recovery,fail_backup_alloc,fail_ota;
static bool relay_outputs[6],led_output;
static unsigned writes,relay_writes,refreshes,reboots,reserve_attempts,backup_allocations,state_locks,ota_calls;
static int childLockEnabled;
static float real_time_power=125;
static unsigned p_count;
static bool tc1_config_reserve(void){reserve_attempts++;if(deny_reserve||reserved)return false;reserved=true;return true;}
static void tc1_config_release(void){assert(reserved);reserved=false;}
static void tc1_status_lock(void){assert(!state_locks);state_locks++;}
static void tc1_status_unlock(void){assert(state_locks==1);state_locks--;}
static bool device_config_recovery(void){return recovery;}
static void device_config_recovery_set(bool b){recovery=b;}
static int mico_system_context_update(void *p){assert(p==sys_config&&reserved&&!state_locks);writes++;return fail_storage?-1:0;}
static int mico_system_context_restore(void *p){assert(p==sys_config);return fail_storage?-1:0;}
static int mico_rtos_create_thread(void *p,int priority,const char *name,void(*fn)(int),unsigned stack,int arg){(void)p;(void)priority;(void)name;(void)fn;(void)stack;(void)arg;reboots++;return 0;}
static void reboot_worker(int arg){(void)arg;}
static bool RelayOut(void){for(unsigned i=0;i<6;i++)if(config.socket_status[i])return true;return false;}
static void UserLedSet(bool enabled){assert(!state_locks);led_output=enabled;}
static void UserRelaySet(unsigned i,bool on){assert(i<6&&!state_locks);config.socket_status[i]=on;relay_outputs[i]=on;relay_writes++;UserLedSet(RelayOut()&&config.power_led_enabled);}
static void UserRelaySetAll(bool on){for(unsigned i=0;i<6;i++)UserRelaySet(i,on);}
static void UserMqttRefreshState(bool discovery){(void)discovery;refreshes++;}
static bool tc1_auth_password_save(const char *p){strcpy(config.web_password,p);return true;}
static bool tc1_auth_reset(bool full){(void)full;return true;}
static char owned_ssid[33],owned_password[65],owned_url[512];
static bool WifiConnect(char *ssid,char *password){strcpy(owned_ssid,ssid);strcpy(owned_password,password);return true;}
static int WifiScanStart(void){return 0;}
static bool WifiScanMp(ph_writer *w){phw_begin(w,NULL,false);phw_bool(w,"scanning",false);phw_begin(w,"networks",true);phw_end(w);phw_end(w);return true;}
static int ota_server_start(char *url,void *md5,void *cb){(void)md5;(void)cb;ota_calls++;strcpy(owned_url,url);return fail_ota?-1:0;}
static float PowerPulseCountToKwh(unsigned n){return n/1000.0f;}
void bemfa_config_get(bemfa_config_t *next){*next=config.bemfa;}
void bemfa_config_mp_fields(ph_writer *w){phw_bool(w,"mqtt_enabled",config.bemfa.mqtt_enabled);}
static void *control_backup_malloc(size_t n){backup_allocations++;return fail_backup_alloc?NULL:malloc(n);}
#define malloc control_backup_malloc
#include "TC1/service_mp.inc"
#undef malloc
static void check(ph_writer *q,const char *action,const char *expected){
 phw_end(q);ph_buf *input=phw_take(q);assert(input);ph_value params;assert(phv_parse(1,input->buf,input->len,&params));
 ph_writer out;assert(phw_init(&out,1));const char *code="unset";
 assert(service_product_mp(action,&params,&out,&code));
 if(strcmp(code,expected))fprintf(stderr,"%s returned %s, expected %s\n",action,code,expected);
 assert(!strcmp(code,expected));
 memset(input->buf,0,input->len);ph_buf_free(input);ph_buf *wire=phw_take(&out);assert(wire);ph_value payload;assert(phv_parse(1,wire->buf,wire->len,&payload));ph_buf_free(wire);
}
static void start(ph_writer *w){assert(phw_init(w,1));phw_begin(w,NULL,false);}
static void control(const char *action,int index,bool enabled,const char *expected){
 ph_writer q;start(&q);phw_bool(&q,!strcmp(action,"outlet.set")?"on":"enabled",enabled);
 if(index>=0)phw_int(&q,"index",index);check(&q,action,expected);assert(!reserved&&!state_locks);
}
static void test_control_transactions(void){
 /* A backup allocation failure must not reject these small transactions.
  * Every control still saves synchronously and rolls hardware back on error. */
 unsigned allocations=backup_allocations;
 fail_backup_alloc=true;
 for(unsigned i=0;i<6;i++)control("outlet.set",i,false,"accepted");
 control("indicator.set",-1,true,"accepted");control("child_lock.set",-1,false,"accepted");
 user_config_t before=config;bool outputs_before[6];memcpy(outputs_before,relay_outputs,sizeof(outputs_before));
 int lock_before=childLockEnabled;bool led_before=led_output;
 unsigned writes_before=writes,relays_before=relay_writes,refreshes_before=refreshes;
 fail_storage=true;
 for(unsigned i=0;i<6;i++)control("outlet.set",i,true,"storage_error");
 control("outlet.set",-1,true,"storage_error");
 control("indicator.set",-1,false,"storage_error");control("child_lock.set",-1,true,"storage_error");
 assert(writes==writes_before+9&&refreshes==refreshes_before);
 assert(!memcmp(&config,&before,sizeof(config))&&childLockEnabled==lock_before);
 assert(!memcmp(relay_outputs,outputs_before,sizeof(outputs_before))&&led_output==led_before);
 assert(relay_writes==relays_before+24&&backup_allocations==allocations);
 /* Repeated values need neither Flash nor a GPIO transition, even while the
  * storage seam rejects all writes. Successful replies still refresh HA. */
 writes_before=writes;relays_before=relay_writes;
 for(unsigned i=0;i<6;i++)control("outlet.set",i,false,"accepted");
 control("outlet.set",-1,false,"accepted");control("indicator.set",-1,true,"accepted");control("child_lock.set",-1,false,"accepted");
 assert(writes==writes_before&&relay_writes==relays_before&&refreshes==refreshes_before+9);
 fail_storage=false;
 control("outlet.set",2,true,"accepted");assert(config.socket_status[2]&&relay_outputs[2]&&led_output);
 control("indicator.set",-1,false,"accepted");assert(!config.power_led_enabled&&!led_output&&relay_outputs[2]);
 control("child_lock.set",-1,true,"accepted");assert(config.user[0]&&childLockEnabled);
 /* Exercise both LED rollback levels with an outlet powered, including a
  * failed bulk-off that must restore the single active relay and its LED. */
 fail_storage=true;control("indicator.set",-1,true,"storage_error");
 assert(!config.power_led_enabled&&!led_output&&relay_outputs[2]);
 fail_storage=false;control("indicator.set",-1,true,"accepted");assert(led_output);
 before=config;refreshes_before=refreshes;relays_before=relay_writes;
 fail_storage=true;control("outlet.set",2,false,"storage_error");control("outlet.set",-1,false,"storage_error");
 control("indicator.set",-1,false,"storage_error");control("child_lock.set",-1,false,"storage_error");
 assert(!memcmp(&config,&before,sizeof(config))&&childLockEnabled&&led_output&&relay_outputs[2]);
 assert(relay_writes==relays_before+4&&refreshes==refreshes_before);
 fail_storage=false;
 assert(backup_allocations==allocations);
 /* Admission still excludes a concurrent OTA/configuration transaction. */
 deny_reserve=true;writes_before=writes;refreshes_before=refreshes;
 control("outlet.set",2,false,"busy");control("indicator.set",-1,true,"busy");control("child_lock.set",-1,false,"busy");
 assert(writes==writes_before&&refreshes==refreshes_before&&relay_outputs[2]&&led_output&&childLockEnabled);
 /* Invalid requests are rejected before touching admission, snapshots or
  * hardware, including when another transaction already owns reservation. */
 unsigned attempts=reserve_attempts;relays_before=relay_writes;
 ph_writer q;
 const char *actions[]={"outlet.set","indicator.set","child_lock.set",NULL};
 for(unsigned i=0;actions[i];i++){
  const char *field=i?"enabled":"on";
  start(&q);check(&q,actions[i],"invalid_params");
  start(&q);phw_int(&q,field,1);check(&q,actions[i],"invalid_params");
  start(&q);phw_bool(&q,field,true);phw_bool(&q,"unknown",true);check(&q,actions[i],"invalid_params");
 }
 for(int index=-1;index<=6;index+=7){start(&q);phw_bool(&q,"on",true);phw_int(&q,"index",index);check(&q,"outlet.set","invalid_params");}
 start(&q);phw_bool(&q,"on",true);phw_bool(&q,"index",true);check(&q,"outlet.set","invalid_params");
 assert(reserve_attempts==attempts&&writes==writes_before&&relay_writes==relays_before&&backup_allocations==allocations);
 deny_reserve=false;fail_backup_alloc=false;
 puts("PASS TC1 controls: no backup allocation, 14 storage rollbacks, idempotent writes, invalid admission and HA refresh");
}
static void section(ph_writer *q,const char *name){start(q);phw_text(q,"section",name);phw_begin(q,"values",false);}
static void mqtt_values(ph_writer *q,const char *host,const char *user,const char *password){
 section(q,"mqtt");phw_text(q,"host",host);phw_int(q,"port",host[0]?1883:0);phw_text(q,"user",user);
 if(password)phw_text(q,"password",password);phw_end(q);
}
static void test_config_scratch_and_rollback(void){
 ph_writer q;char host[128],user[64],password[257];
 memset(host,'h',127);host[127]=0;memset(user,'u',63);user[63]=0;memset(password,'p',256);password[256]=0;
 mqtt_values(&q,host,user,password);check(&q,"config.set","accepted");
 assert(!strcmp(config.mqtt_host,host)&&!strcmp(config.mqtt_user,user)&&!strcmp(config.mqtt_password,password));
 user_config_t before=config;unsigned saved=writes,published=refreshes;
 fail_storage=true;mqtt_values(&q,"new-broker","new-user","new-secret");check(&q,"config.set","storage_error");
 assert(!memcmp(&before,&config,sizeof(config))&&writes==saved+1&&refreshes==published);fail_storage=false;
 mqtt_values(&q,"broker","user",NULL);check(&q,"config.set","accepted");assert(!strcmp(config.mqtt_password,password));
 mqtt_values(&q,"","","");check(&q,"config.set","accepted");assert(!config.mqtt_host[0]&&!config.mqtt_password[0]&&!config.mqtt_port);
 before=config;saved=writes;published=refreshes;
 section(&q,"mqtt");phw_text(&q,"host","broker");phw_int(&q,"port",0);phw_text(&q,"user","");phw_end(&q);check(&q,"config.set","invalid_params");
 assert(!memcmp(&before,&config,sizeof(config))&&writes==saved&&refreshes==published);

 /* Use the real 592-byte config and transactional validator. Boundary-sized
  * credentials survive the request buffer being poisoned/freed by check(). */
 char app[65],secret[129];memset(app,'a',64);app[64]=0;memset(secret,'s',128);secret[128]=0;
 unsigned revision=config.bemfa.revision;
 section(&q,"bemfa");phw_bool(&q,"enabled",true);phw_text(&q,"app_id",app);phw_text(&q,"secret_key",secret);phw_end(&q);check(&q,"config.set","accepted");
 assert(config.bemfa.enabled&&config.bemfa.revision==revision+1&&!strcmp(config.bemfa.app_id,app)&&!strcmp(config.bemfa.secret_key,secret));
 before=config;saved=writes;published=refreshes;
 for(unsigned failure=0;failure<4;failure++){
  section(&q,"bemfa");
  if(failure==0)phw_int(&q,"enabled",1);
  else if(failure==1)phw_text(&q,"unknown","rejected");
  else{
   phw_begin(&q,"topics",true);
   for(unsigned i=0;i<(failure==2?5:6);i++)phw_text(&q,NULL,"duplicate001");
   phw_end(&q);
  }
  phw_end(&q);check(&q,"config.set","invalid_params");
  assert(!memcmp(&before,&config,sizeof(config))&&writes==saved&&refreshes==published);
 }
 fail_storage=true;section(&q,"bemfa");phw_text(&q,"secret_key","replacement");phw_end(&q);check(&q,"config.set","storage_error");
 assert(!memcmp(&before,&config,sizeof(config))&&writes==saved+1&&refreshes==published);fail_storage=false;
 section(&q,"bemfa");phw_text(&q,"app_id","new-app");phw_end(&q);check(&q,"config.set","accepted");
 assert(config.bemfa.revision==before.bemfa.revision+1&&!strcmp(config.bemfa.app_id,"new-app")&&!strcmp(config.bemfa.secret_key,secret));
 assert(!reserved&&!state_locks);
 puts("PASS TC1 actual Bemfa validator and MQTT scratch: boundary credentials, omitted secrets, rejected maps and persistence rollback");
}
static void test_ota_scratch(void){
 ph_writer q;char url[513];memset(url,'u',sizeof(url));url[511]=0;
 unsigned calls=ota_calls,saved=writes;
 start(&q);phw_text(&q,"url",url);check(&q,"ota.start","accepted");assert(ota_calls==calls+1&&!strcmp(owned_url,url));
 fail_ota=true;start(&q);phw_text(&q,"url","busy");check(&q,"ota.start","busy");fail_ota=false;assert(ota_calls==calls+2);
 start(&q);check(&q,"ota.start","invalid_params");
 start(&q);phw_int(&q,"url",1);check(&q,"ota.start","invalid_params");
 start(&q);phw_text(&q,"url","valid");phw_bool(&q,"unknown",true);check(&q,"ota.start","invalid_params");
 url[511]='u';url[512]=0;start(&q);phw_text(&q,"url",url);check(&q,"ota.start","invalid_params");
 assert(ota_calls==calls+2&&writes==saved&&!reserved&&!state_locks);
 puts("PASS TC1 OTA scratch: owned 511-byte URL, overlength/type/key rejection and busy propagation");
}
int main(void){
 bemfa_defaults(&config.bemfa,"tc1",str_mac);
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
 test_control_transactions();
 test_config_scratch_and_rollback();test_ota_scratch();
 return 0;
}

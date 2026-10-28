"""Execute production M1 persistence, command routing and UART worker with faults."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def run_host(source):
    from tests.mp_fixture_vectors import header
    with tempfile.TemporaryDirectory() as directory:
        tmp = Path(directory)
        (tmp / 'mp_fixture_vectors.inc').write_text(header([source], {}))
        (tmp / 'test.c').write_text(source)
        command = [os.environ.get('CC', 'cc'), '-std=gnu99', '-g', '-fsanitize=undefined,address', '-no-pie',
                   '-I'+str(ROOT), '-I'+str(ROOT/'M1'), '-I'+str(ROOT/'tests/host'), '-I'+str(tmp),
                   '-Wno-pointer-sign', str(tmp/'test.c'), str(ROOT/'M1/protocol.c'),
                   str(ROOT/'common/typed_wire.c'), str(ROOT/'common/wire_buffer.c'), '-lm', '-o', str(tmp/'test')]
        subprocess.run(command, check=True)
        subprocess.run([str(tmp/'test')], check=True)



class StockRuntimeTest(unittest.TestCase):
    def test_m1_storage_commands_and_uart(self):
        main = (ROOT / 'M1/main.c').read_text()
        production = main[main.index('static bool config_recovery'):main.index('#include "../common/service.h"')] + main[main.index('mico_mutex_t m1_lock'):main.index('int application_start')]
        migration = main[main.index('    bool migration_changed'):main.index('    err = mico_system_init')]
        harness = r'''
#include "M1/m1.h"
#include "common/device_status.h"
#include "M1/config_legacy.h"
#include "M1/storage.h"
#include "common/automation.h"
#include <setjmp.h>
static unsigned now,writes;
static bool fail_flash;
static int flash_update(void *p);
#define mico_system_context_update flash_update
void feature_defaults(feature_config_t *c){memset(c,0,sizeof(*c));c->magic=FEATURE_MAGIC;}
void automation_defaults(automation_config_t *c){memset(c,0,sizeof(*c));c->magic=AUTOMATION_MAGIC;}
PRODUCTION
static app_storage_t live,flash;
static int flash_update(void *p){(void)p;writes++;if(fail_flash)return -1;flash=live;return 0;}
void service_event(const char *level,const char *module,const char *name){(void)level;(void)module;(void)name;}
static int migrate(void){m1_config_t c;MIGRATION return config_recovery?-1:0;}
static unsigned step,queued,version_queries,buzzer_on,buzzer_off,stop_ms;
static unsigned wake_beeps, beep_times[8], screen_rx;
static unsigned tone_beeps,tone_stops,tone_attempts,tone_times[8],music_plays,music_stops;
static uint8_t music_args[7];
static unsigned threshold_attempts, threshold_success, threshold_first, threshold_last;
static bool fail_threshold;
static uint8_t threshold_frame[12];
static unsigned alarm_writes,alarm_reads,alarm_io_fault;
static uint8_t alarm_device_mask;
static unsigned reset_rx,reset_acks,reset_writes;
static uint8_t reset_ack[12];
static jmp_buf done;
uint32_t mico_rtos_get_time(void){return now;}
static unsigned phase;
void mico_thread_msleep(unsigned ms){if(phase>=8){if(phase<12)now+=ms;m1_mcu_tick();}}
void mico_rtos_thread_sleep(unsigned sec){(void)sec;}
void watchdog_beat(int slot){(void)slot;}
typedef struct { int unused; } ring_buffer_t;
static void ring_buffer_init(ring_buffer_t *r,uint8_t *p,size_t n){(void)r;(void)p;(void)n;}
typedef struct {unsigned baud,a,b,c,d;} mico_uart_config_t;
enum {MICO_UART_1,MICO_PWM_4,MICO_PWM_5,MICO_GPIO_13,MICO_GPIO_14,OUTPUT_PUSH_PULL,DATA_WIDTH_8BIT,NO_PARITY,STOP_BITS_1,FLOW_CONTROL_DISABLED};
typedef int mico_pwm_t;
static int MicoUartInitialize(int port,mico_uart_config_t *c,ring_buffer_t *r){(void)port;(void)r;assert(c->baud==115200);return 0;}
static int MicoUartSend(int port,const uint8_t *p,unsigned n){
 (void)port;assert((n==12 || p[1]==0x13) && p[0]==0x23);
 if(phase==18 && p[1]==0x16){
  const uint8_t ack[12]={0x23,0x16,0,0x10,0x14,0,3,0x10,0xf1,0xff,0xff,0x21};
  assert(!memcmp(p,ack,12) && step>=3 && m1_state.reboot_pending);
  assert(!flash.base.ssid[0] && !flash.base.wifi_password[0] && !flash.base.web_password[0]);
  assert(!strcmp(flash.base.mqtt_host,"keep-broker") && !strcmp(flash.base.mqtt_password,"keep-key"));
  assert(writes==reset_writes+2); /* A failed storage attempt followed by one commit. */
  reset_acks++;return reset_acks==1?-1:0;
 }
 if(phase==20 && p[1]==0x72){
  memcpy(reset_ack,p,12);reset_acks++;return reset_acks==1?-1:0;
 }
 if(phase==17 && p[1]==0x70 && p[2]==20){
  assert(m1_state.mcu_alarm_schema==1 && p[4]<=1 && !(p[5]&~7u));
  for(unsigned i=6;i<11;i++)assert(!p[i]);
  if(p[4])alarm_writes++;else {alarm_reads++;assert(!p[5]);}
  unsigned fault=alarm_io_fault;alarm_io_fault=0;
  if(fault==1)return -1;
  uint8_t ack[20]={0x23,0x70,20,p[3],0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0x21};
  if(fault==2)ack[4]=2;
  else if(p[4])alarm_device_mask=p[5];
  if(fault==3)return 0;
  ack[6]=fault==4?(alarm_device_mask^1):fault==5?8:alarm_device_mask;
  if(fault==6)ack[7]=2;if(fault==7)ack[5]=2;
  m1_mcu_rx(ack);return 0;
 }
 if(phase>=12 && p[1]==0x70 && p[2]==14){
  tone_attempts++;
  if(phase==14 && tone_attempts==1)return -1;
  if(p[4]){assert(p[4]==3 && tone_beeps<8);tone_times[tone_beeps++]=now;}else tone_stops++;
 }
 if(phase>=15 && p[1]==0x70 && p[2]==15){
  memcpy(music_args,p+4,7);if(p[4]==5)music_plays++;if(p[4]==6)music_stops++;
 }
 if(phase>=8 && phase!=10){
  uint8_t ack[20]={0x23,0x70,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x21};
  if(p[1]==0x70){ack[2]=p[2];ack[3]=p[3];
   if(p[2]==0){ack[5]=phase==11?1:2;ack[6]=phase>=15?0xff:phase>=12?0x7f:0x3f;ack[7]=0xff;ack[10]=phase>=12;ack[17]=phase==17;}
   if(p[2]==15 && !p[4]){ack[5]=1;ack[7]=0;ack[8]=8;ack[18]=1;}
   if(p[2]==13){ack[13]=0x02;ack[14]=0xbc;ack[15]=1;ack[16]=1;ack[18]=20;}
  }else if(p[1]==0x12){ack[1]=0x14;}
  else if(p[1]==0x13){ack[1]=0x14;ack[2]=p[2];ack[3]=p[3];ack[4]=1;}
  else if(p[1]==0x16){ack[1]=0x1a;}
  m1_mcu_rx(ack);return 0;
 }
 if(p[1]==5 || p[1]==6){
  threshold_attempts++;memcpy(threshold_frame,p,12);
  if(fail_threshold){fail_threshold=false;return -1;}
  threshold_success++;if(threshold_success==1)threshold_first=now;threshold_last=now;
 }
 if(p[1]==17)version_queries++;
 if(p[1]==14){if(p[2]){buzzer_on++;if(phase){assert(wake_beeps<8);beep_times[wake_beeps++]=now;}}else {buzzer_off++;if(now)stop_ms=now;}}
 return 0;
}
static unsigned rx;
static int MicoUartRecv(int port,uint8_t *out,unsigned n,unsigned timeout){
 (void)port;(void)n;(void)timeout;
 const uint8_t reply[20]={0x23,17,3,'1','.','2',0,0,0,0,0,0,0,0,0,0,0,0,0,0x21};
 if(!phase && step==5 && rx<20){*out=reply[rx++];return 0;}
 if((phase==2 || phase==3) && step==2 && screen_rx<20){
  uint8_t event[20]={0x23,0x0f,phase==3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x21};
  *out=event[screen_rx++];return 0;
 }
 if(phase==18 && step>=1 && step<=4 && reset_rx<(step*20)){
  const uint8_t event[20]={0x23,0x18,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x21};
  *out=event[reset_rx++%20];return 0;
 }
 if(phase==19 && step>=2 && screen_rx<40){
  const uint8_t click[20]={[0]=0x23,[1]=0x71,[2]=1,[3]=1,[19]=0x21};
  const uint8_t screen[20]={[0]=0x23,[1]=0x0f,[19]=0x21};
  *out=screen_rx<20?click[screen_rx]:screen[screen_rx-20];screen_rx++;return 0;
 }
 return -1;
}
static int MicoPwmStop(int p){(void)p;return 0;}
static int MicoPwmStart(int p){(void)p;return 0;}
static int MicoPwmInitialize(int p,unsigned hz,float duty){(void)p;(void)hz;(void)duty;return 0;}
static int MicoGpioInitialize(int p,int m){(void)p;(void)m;return 0;}
static int MicoGpioOutputLow(int p){(void)p;return 0;}
unsigned feature_light_cap(unsigned b){return b;}
static int queue_push(mico_queue_t *q,void *p,unsigned timeout){(void)q;(void)p;(void)timeout;if(queued==10)return -1;queued++;return 0;}
static int queue_pop(mico_queue_t *q,void *p,unsigned timeout){(void)q;(void)timeout;if(!queued)return -1;queued--;memset(p,0,14);return -1;}
#define mico_rtos_push_to_queue queue_push
#define mico_rtos_pop_from_queue queue_pop
void m1_barometer_tick(void){}
void m1_scd40_tick(void);
#include "M1/uart.c"
#include "M1/mcu.c"
#include "M1/co2_source.c"
#include "M1/display_source.c"
void m1_scd40_tick(void){
 step++;now+=100;
 if(phase==18){
  if(step==2)m1_state.ota_active=false;
  if(step==3)fail_flash=false;
  if(step==5)longjmp(done,1);
 }else if(phase==19){
  if(step==4)longjmp(done,1);
 }else if(phase>=12){
  if(step==1)assert(m1_wake_alarm_tone_start(5,phase==16?5:3));
  if(step==2 && phase==13)m1_state.mcu_tones=false;
  if(step==22 && phase!=13)m1_wake_alarm_stop();
  if(step==60)longjmp(done,1);
 }else if(phase==7){
  queued=10;
  if(step==25)longjmp(done,1);
 }else if(!phase){
  if(step==1){assert(m1_uart_buzzer(true,300));queued=10;assert(!m1_uart_version_query());}
  if(step==30)longjmp(done,1);
 }else{
  if(step==1){assert(m1_wake_alarm_start(5));assert(!m1_wake_alarm_start(5));queued=10;}
  if(step==2 && phase==4)m1_wake_alarm_stop();
  if(step==2 && phase==5)m1_state.ota_active=true;
  if(step==60)longjmp(done,1);
 }
}
bool m1_scan_start(void){return false;}
bool m1_wifi_trial(const m1_config_t *c){(void)c;return false;}
bool m1_ota_download(const char *s){(void)s;return false;}
#define HOST_M1 1
int mico_rtos_create_thread(void *t,int p,const char *n,void (*f)(mico_thread_arg_t),unsigned stack,int arg){(void)t;(void)p;(void)n;(void)f;(void)stack;(void)arg;return 0;}
bool feature_mp(const char*a,const ph_value*p,ph_writer*w,const char**c){(void)a;(void)p;(void)w;(void)c;return false;}
bool feature_ntp_set_mp(const char*h,const char**c){(void)h;(void)c;return false;}
void service_cache_changed(unsigned m){(void)m;}
bool service_control_begin(void){return true;}void service_control_end(void){}
void m1_status_capture(device_status_t*a,device_status_full_t*b){(void)a;(void)b;abort();}
void device_status_mp(ph_writer*w,const device_status_t*a,const device_status_full_t*b){(void)w;(void)a;(void)b;abort();}
#include "common/service_product.inc"
#include "mp_fixture_vectors.inc"
#include "tests/host/stock_mp_assert.h"
static const char *command(const char *a,const char *params){
 ph_value p;bool found=false;for(unsigned i=0;i<sizeof(fixture_vectors)/sizeof(*fixture_vectors);i++)if(!strcmp(params,fixture_vectors[i].text)){assert(phv_parse(PH_WIRE_MSGPACK,fixture_vectors[i].bytes,fixture_vectors[i].length,&p));found=true;break;}assert(found);
 ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));const char *code;assert(service_product_mp(a,&p,&w,&code));ph_buf*b=phw_take(&w);assert(b);ph_buf_free(b);return code;
}
int main(void){
 app_config=&live.base;appRestoreDefault_callback(&live,sizeof(live));
 assert(!m1_uart_buzzer(true,100));queue_ready=true;
 assert(!m1_uart_buzzer(true,0) && !m1_uart_buzzer(true,5001));
 assert(!strcmp(command("buzzer.set","{\"enabled\":true,\"duration_ms\":5001}"),"invalid_params"));
 m1_state.ota_active=true;assert(!strcmp(command("buzzer.set","{\"enabled\":true}"),"busy"));
 m1_state.ota_active=false;
 if(!setjmp(done))uart_worker(0);
 assert(buzzer_on==1 && buzzer_off==2 && stop_ms==400 && !m1_state.buzzer_requested);
 assert(m1_state.mcu_version_received && !strcmp(m1_state.mcu_version,"1.2") && version_queries==1);
 /* Same production UART worker: repeated pulses, screen events in both
  * directions, explicit stop, OTA cancellation, and uint32 uptime wrap. */
 assert(!m1_wake_alarm_start(0) && !m1_wake_alarm_start(181));
 now=100;assert(!strcmp(command("wake_alarm.start","{}"),"accepted"));
 assert(m1_state.wake_alarm_until==60100);assert(!strcmp(command("wake_alarm.get","{}"),"ok"));
 assert(!strcmp(command("wake_alarm.stop","{}"),"accepted") && !m1_state.wake_alarm_active);
 const char *invalid_wake[]={"{\"duration_seconds\":0}","{\"duration_seconds\":181}","{\"duration_seconds\":true}","{\"duration_seconds\":1.5}","{\"duration_seconds\":5,\"unknown\":1}"};
 for(unsigned j=0;j<sizeof(invalid_wake)/sizeof(*invalid_wake);j++)assert(!strcmp(command("wake_alarm.start",invalid_wake[j]),"invalid_params"));
 for(phase=1;phase<=6;phase++){
  now=phase==6?UINT32_MAX-1000:0;unsigned start=now;
  step=queued=screen_rx=wake_beeps=0;m1_state.ota_active=false;
  if(!setjmp(done))uart_worker(0);
  assert(!m1_state.wake_alarm_active && !m1_state.buzzer_requested);
  assert(wake_beeps==((phase<=3 || phase==6)?3:1)); /* Screen changes do not cancel alarms. */
  assert((unsigned)(beep_times[0]-start)==100);
  if(wake_beeps==3)assert((unsigned)(beep_times[1]-start)==2100 && (unsigned)(beep_times[2]-start)==4100);
 }
 m1_state.ota_active=true;
 assert(!strcmp(command("wake_alarm.start","{}"),"busy"));
 assert(!strcmp(command("wake_alarm.stop","{}"),"accepted"));
 /* Stop remains a local safety operation under every network/OTA gate. */
 bool *stop_busy[]={&m1_state.ota_active,&m1_state.mcu_updating,&m1_state.wifi_trial_active,&m1_state.reboot_pending};
 m1_state.ota_active=false;
 for(unsigned i=0;i<4;i++){
  *stop_busy[i]=true;m1_state.wake_alarm_active=true;m1_state.wake_alarm_until=now+60000;
  assert(!strcmp(command("wake_alarm.stop","{\"unexpected\":1}"),"invalid_params") && m1_state.wake_alarm_active);
  assert(!strcmp(command("wake_alarm.stop","{}"),"accepted") && !m1_state.wake_alarm_active);
  *stop_busy[i]=false;
 }
 for(unsigned version=1;version<=5;version++){
  appRestoreDefault_callback(&live,sizeof(live));live.base.version=version;strcpy(live.base.ssid,"keep");
  live.features.magic=FEATURE_MAGIC;live.automation.magic=AUTOMATION_MAGIC;
  memset(live.reserved_controller_config,0xa5,sizeof(live.reserved_controller_config));
  if(version<3){m1_storage_v2_t old={0};old.base.magic=M1_CONFIG_MAGIC;old.base.version=version;strcpy(old.base.ssid,"keep");memcpy(&live,&old,sizeof(old));}
  assert(!migrate() && live.base.version==M1_CONFIG_VERSION);
  assert(!strcmp(live.base.ssid,"keep"));
  for(unsigned j=0;j<20;j++)assert(live.reserved_controller_config[j]==0);
 }
 live.base.version=5;fail_flash=true;assert(migrate()!=0);fail_flash=false;config_recovery=false;
 memset(live.reserved_controller_config,0xa5,20);live.base.version=M1_CONFIG_VERSION;unsigned before=writes;
 assert(!migrate() && writes==before);

 /* Threshold-only v6 records keep limits/revisions and acquire an OFF default
  * in RAM without rewriting their storage bytes. Unknown alarm schemas do too. */
 m1_alarm_config_t legacy_alarm={.magic=M1_ALARM_MAGIC,.revision=17,.hcho={80,220},.pm25={35,70},.configured=3};
 memcpy(live.reserved_controller_config,&legacy_alarm,20);app_storage_t legacy_before=live;
 m1_alarm_config_t migrated_alarm;m1_alarm_get(&migrated_alarm);
 assert(migrated_alarm.hcho[0]==80 && migrated_alarm.pm25[1]==70 && migrated_alarm.revision==17);
 assert(migrated_alarm.reserved[0]==M1_ALARM_SCHEMA && !migrated_alarm.reserved[1]);
 assert(!memcmp(&legacy_before,&live,sizeof(live)));
 for(unsigned kind=0;kind<3;kind++){
  legacy_alarm.reserved[0]=kind==0?2:M1_ALARM_SCHEMA;
  legacy_alarm.reserved[1]=kind==1?8:3;legacy_alarm.reserved[2]=kind==2;
  memcpy(live.reserved_controller_config,&legacy_alarm,20);m1_alarm_get(&migrated_alarm);
  assert(!migrated_alarm.reserved[1] && migrated_alarm.hcho[0]==80 && migrated_alarm.pm25[1]==70);
 }

 /* Restored stock threshold API: persistence, admission and UART-only proof. */
 appRestoreDefault_callback(&live,sizeof(live));memset(&m1_state,0,sizeof(m1_state));
 memset(live.reserved_controller_config,0xa5,20);
 m1_alarm_config_t blank;m1_alarm_get(&blank);assert(!blank.configured && !blank.revision && !blank.reserved[1]);
 unsigned blank_attempts=threshold_attempts;alarm_sync();assert(threshold_attempts==blank_attempts);
 fixture_value *caps=service_product_capabilities();
 const char *caps_text=fixture_description(caps);assert(strstr(caps_text,"alarm.get") && strstr(caps_text,"alarm.set"));fixture_release(caps);
 assert(!strcmp(command("alarm.get","{}"),"ok"));
 assert(!strcmp(command("alarm.get","{\"other\":1}"),"invalid_params"));
 assert(!strcmp(command("alarm.set","{}"),"invalid_params"));
 const char *bad_alarm[]={
  "{\"sensor\":\"co2\",\"yellow\":1,\"red\":2,\"revision\":0}",
  "{\"sensor\":\"hcho\",\"yellow\":3,\"red\":2,\"revision\":0}",
  "{\"sensor\":\"hcho\",\"yellow\":0,\"red\":0,\"revision\":0}",
  "{\"sensor\":\"hcho\",\"yellow\":true,\"red\":2,\"revision\":0}",
  "{\"sensor\":\"hcho\",\"yellow\":1.5,\"red\":2,\"revision\":0}",
  "{\"sensor\":\"hcho\",\"yellow\":1,\"red\":65536,\"revision\":0}",
  "{\"sensor\":\"hcho\",\"yellow\":1,\"red\":2,\"revision\":-1}",
  "{\"sensor\":\"hcho\",\"yellow\":1,\"red\":2,\"revision\":0,\"other\":1}",
  "{\"sensor\":\"hcho\",\"yellow\":1,\"red\":2,\"revision\":0,\"buzzer_enabled\":1}",
  "{\"sensor\":\"hcho\",\"yellow\":1,\"red\":2,\"revision\":0,\"buzzer_enabled\":\"false\"}",
  "{\"sensor\":\"hcho\",\"yellow\":1,\"red\":2,\"revision\":0,\"buzzer_enabled\":null}",
  "{\"sensor\":\"hcho\",\"yellow\":1,\"red\":2,\"revision\":0,\"buzzer_enabled\":[]}",
  "{\"sensor\":\"hcho\",\"yellow\":1,\"red\":2,\"revision\":0,\"buzzer_mask\":8}"
 };
 for(unsigned j=0;j<sizeof(bad_alarm)/sizeof(*bad_alarm);j++)assert(!strcmp(command("alarm.set",bad_alarm[j]),"invalid_params"));
 const char *set_hcho="{\"sensor\":\"hcho\",\"yellow\":80,\"red\":100,\"revision\":0}";
 m1_state.ota_active=true;assert(!strcmp(command("alarm.set",set_hcho),"busy"));m1_state.ota_active=false;
 m1_state.wifi_trial_active=true;assert(!strcmp(command("alarm.set",set_hcho),"busy"));m1_state.wifi_trial_active=false;
 m1_state.reboot_pending=true;assert(!strcmp(command("alarm.set",set_hcho),"busy"));m1_state.reboot_pending=false;
 app_storage_t preserved=live;
 fail_flash=true;assert(!strcmp(command("alarm.set",set_hcho),"storage_error"));fail_flash=false;
 assert(!memcmp(&preserved,&live,sizeof(live)));
 assert(!strcmp(command("alarm.set",set_hcho),"accepted"));
 m1_alarm_config_t a;m1_alarm_get(&a);
 assert(a.configured==1 && a.revision==1 && a.hcho[0]==80 && a.hcho[1]==100);
 assert(!memcmp(&live,&preserved,offsetof(app_storage_t,reserved_controller_config)));
 assert(!strcmp(command("alarm.set",set_hcho),"busy"));
 live=flash; /* Reboot uses persisted config, including a single configured sensor. */
 memset(&m1_state,0,sizeof(m1_state));m1_alarm_get(&a);assert(a.revision==1);
 threshold_attempts=threshold_success=0;alarm_sync();
 assert(threshold_attempts==1 && threshold_frame[1]==5 && threshold_frame[3]==80 && threshold_frame[5]==100);
 alarm_sync();assert(threshold_attempts==1); /* no repeated successful write */
 fixture_value *out=m1_alarm_json();assert(fixture_bool(fixture_get(out,"uart_sent")));
 assert(!strcmp(fixture_text(fixture_get(out,"confirmation")),"uart_send_only"));fixture_release(out);
 assert(!strcmp(command("alarm.set","{\"sensor\":\"pm25\",\"yellow\":4660,\"red\":43981,\"revision\":1}"),"accepted"));
 m1_alarm_get(&a);assert(a.configured==3 && a.hcho[0]==80 && a.pm25[1]==43981);
 assert(!m1_state.alarm_uart_sent);
 fail_threshold=true;alarm_sync();assert(!m1_state.alarm_uart_sent);
 alarm_sync();assert(m1_state.alarm_uart_sent && threshold_frame[1]==6 && threshold_frame[2]==0x12 && threshold_frame[3]==0x34 && threshold_frame[4]==0xab && threshold_frame[5]==0xcd);
 unsigned attempts=threshold_attempts;
 m1_state.alarm_uart_sent=false;m1_state.ota_active=true;alarm_sync();assert(threshold_attempts==attempts);m1_state.ota_active=false;
 /* Real worker retries every second outside the saturated command FIFO. */
 now=UINT32_MAX-500;step=0;phase=7;queued=10;
 threshold_attempts=threshold_success=0;fail_threshold=true;
 if(!setjmp(done))uart_worker(0);
 assert(threshold_attempts==4 && threshold_success==3 && m1_state.alarm_uart_sent);
 assert((unsigned)(threshold_first-(UINT32_MAX-500))==0);
 assert((unsigned)(threshold_last-(UINT32_MAX-500))==1000);
 m1_state.reboot_pending=false;assert(m1_reset(false));m1_alarm_get(&a);assert(a.configured==3);
 m1_state.reboot_pending=false;assert(m1_reset(true));m1_alarm_get(&a);assert(!a.configured && !a.revision);
 attempts=threshold_attempts;alarm_sync();assert(threshold_attempts==attempts);


 /* Extension detection, configured screens and actual HTTP/UART exchange. */
 memset(&m1_state,0,sizeof(m1_state));m1_state.uart_ready=true;
 m1_state.mcu_version_received=true;strcpy(m1_state.mcu_version,"0.9.1-test");
 m1_co2_config_t c;m1_co2_config_get(&c);assert(c.magic==M1_CO2_MAGIC && c.mode==0 && c.low==1000 && c.high==1500);
 const char *screen_set="{\"mode\":2,\"slot\":0,\"interval_seconds\":3,\"low\":1000,\"high\":1500,\"revision\":0}";
 assert(!strcmp(command("co2_display.set",screen_set),"unsupported"));
 now=10000;phase=8;auto_waiting=false;auto_due=manual_until=0;m1_mcu_tick();assert(m1_state.mcu_extended && m1_state.mcu_protocol==2);
 assert(!strcmp(command("co2_display.set",screen_set),"unsupported"));
 strcpy(m1_state.scd40.serial,"test-module");
 fail_flash=true;assert(!strcmp(command("co2_display.set",screen_set),"storage_error"));fail_flash=false;
 m1_co2_config_get(&c);assert(!c.mode && !c.revision);
 assert(!strcmp(command("co2_display.set",screen_set),"accepted"));
 assert(!strcmp(command("co2_display.set",screen_set),"busy"));
 live=flash;m1_co2_config_get(&c);assert(c.mode==2 && c.interval==3 && c.revision==1);
 m1_state.scd40.ready=m1_state.scd40.received=true;m1_state.scd40.co2_ppm=700;m1_state.scd40.updated_ms=now;
 for(unsigned i=0;i<4;i++){now+=10;m1_mcu_tick();}
 out=m1_co2_json();assert(fixture_bool(fixture_get(out,"available")) && fixture_bool(fixture_get(out,"mcu_data_valid")));fixture_release(out);
 uint8_t req[12]={0x23,0x70,0,77,0,0,0,0,0,0,0,0x21},reply[20];
 assert(m1_mcu_exchange(req,12,reply) && reply[3]==77 && reply[5]==2);
 m1_state.ota_active=true;assert(!m1_mcu_exchange(req,12,reply));m1_state.ota_active=false;
 req[1]=0x44;assert(!m1_mcu_exchange(req,12,reply));req[1]=0x70;
 phase=10;unsigned timeout_at=now;assert(!m1_mcu_exchange(req,12,reply));assert(now-timeout_at>=2500 && !exchange_pending);
 phase=8;req[1]=0x12;req[2]=1;req[3]=0;
 assert(m1_mcu_exchange(req,12,reply) && reply[1]==0x14 && m1_state.mcu_updating);
 req[1]=0x16;req[2]=0;assert(!m1_mcu_exchange(req,12,reply)); /* premature commit */
 uint8_t data[6]={0x23,0x13,0,2,0,0};assert(m1_mcu_exchange(data,6,reply) && reply[1]==0x14 && reply[3]==2);
 uint8_t complete[20]={0x23,0x15,1};m1_mcu_rx(complete);
 assert(m1_mcu_exchange(req,12,reply) && reply[1]==0x16);
 now+=16000;m1_mcu_tick();assert(!m1_state.mcu_updating);
 phase=11;auto_waiting=false;auto_due=manual_until=0;m1_state.mcu_extended=false;m1_mcu_tick();assert(!m1_state.mcu_extended);
 live.base.version=6;memset(&live.co2,0xa5,sizeof(live.co2));
 uint8_t keep_alarm[20];memcpy(keep_alarm,live.reserved_controller_config,20);
 assert(!migrate() && live.base.version==M1_CONFIG_VERSION && live.co2.mode==0 && live.co2.interval==5);
 assert(!memcmp(keep_alarm,live.reserved_controller_config,20));
 /* Version AND queried extension/PWM capabilities gate all custom tones. */
 memset(&m1_state,0,sizeof(m1_state));m1_state.uart_ready=true;m1_state.mcu_version_received=true;
 strcpy(m1_state.mcu_version,"0.9.2-abcdef");now=10000;phase=12;
 auto_waiting=false;auto_due=manual_until=0;m1_mcu_tick();assert(m1_state.mcu_tones && !m1_tone_supported());
 assert(!strcmp(command("wake_alarm.start","{\"tone\":3}"),"unsupported"));
 strcpy(m1_state.mcu_version,"0.9.3-abcdef");assert(m1_tone_supported());
 m1_state.mcu_tones=false;assert(!strcmp(command("wake_alarm.start","{\"tone\":3}"),"unsupported"));
 m1_state.mcu_tones=true;assert(!strcmp(command("wake_alarm.start","{\"tone\":6}"),"invalid_params"));
 assert(!strcmp(command("wake_alarm.start","{\"tone\":true}"),"invalid_params"));
 assert(!strcmp(command("wake_alarm.start","{\"tone\":3,\"duration_seconds\":1}"),"accepted"));m1_wake_alarm_stop();
 uint8_t reboot[20]={0x23,0x17};m1_mcu_rx(reboot);assert(!m1_tone_supported());
 /* Production worker: two-second preset cadence, stop, capability loss,
  * bounded TX retry and uint32 rollover, all outside the saturated FIFO. */
 for(unsigned variant=12;variant<=14;variant++)for(unsigned wrap=0;wrap<2;wrap++){
  memset(&m1_state,0,sizeof(m1_state));m1_state.mcu_version_received=m1_state.mcu_tones=true;
  strcpy(m1_state.mcu_version,"0.9.3-abcdef");
  now=wrap?UINT32_MAX-500:0;unsigned start=now;phase=variant;step=0;queued=10;
  tone_beeps=tone_stops=tone_attempts=0;auto_waiting=false;manual_until=auto_due=now+60000;
  if(!setjmp(done))uart_worker(0);
  assert(!m1_state.wake_alarm_active && tone_stops==1);
  assert(tone_beeps==(variant==13?1:2));
  assert((unsigned)(tone_times[0]-start)==(variant==14?200:100));
  if(variant!=13)assert((unsigned)(tone_times[1]-start)==2100);
 }
 /* Real product dispatch: music needs both version and bit 15. */
 phase=15;now=10000;auto_waiting=false;manual_until=auto_due=0;
 m1_state.mcu_version_received=true;strcpy(m1_state.mcu_version,"0.9.3-abcdef");
 m1_state.mcu_tones=true;m1_state.mcu_caps=0x7fff;
 assert(!strcmp(command("music.get","{}"),"unsupported"));
 m1_state.mcu_caps=0xffff;assert(!strcmp(command("music.get","{}"),"ok") && m1_state.mcu_music_present);
 assert(!strcmp(command("music.begin","{\"length\":8,\"crc32\":\"1234abcd\"}"),"accepted"));
 assert(music_args[0]==1 && music_args[2]==8 && music_args[3]==0x12 && music_args[6]==0xcd);
 assert(!strcmp(command("music.chunk","{\"offset\":4,\"data\":\"c,d\"}"),"accepted"));
 assert(music_args[0]==2 && music_args[2]==4 && music_args[3]=='c' && !music_args[6]);
 assert(!strcmp(command("music.begin","{\"length\":1025,\"crc32\":\"1234abcd\"}"),"invalid_params"));
 assert(!strcmp(command("music.begin","{\"length\":8,\"crc32\":\"invalid!\"}"),"invalid_params"));
 assert(!strcmp(command("music.chunk","{\"offset\":0,\"data\":\"abcde\"}"),"invalid_params"));
 /* Song alarm is started once (not every two seconds), then explicitly stopped. */
 memset(&m1_state,0,sizeof(m1_state));m1_state.mcu_version_received=m1_state.mcu_tones=m1_state.mcu_music_present=true;
 m1_state.mcu_caps=0xffff;strcpy(m1_state.mcu_version,"0.9.3-abcdef");
 now=0;step=0;phase=16;queued=10;music_plays=music_stops=0;auto_waiting=false;manual_until=auto_due=60000;
 if(!setjmp(done))uart_worker(0);
 assert(music_plays==1 && music_stops==1 && !m1_state.wake_alarm_active);

 /* Details action works on old MCU without inventing measurements. */
 memset(&details,0,sizeof(details));
 assert(!strcmp(command("sensor_details.get","{}"),"ok"));
 assert(!strcmp(command("sensor_details.get","{\"unexpected\":1}"),"invalid_params"));
 out=m1_sensor_details_json();assert(fixture_description(out));assert(!fixture_bool(fixture_get(out,"available")));assert(!fixture_get(out,"pm"));fixture_release(out);
 /* Actual UART capability response, not a version comparison, opens OP10. */
 auto_waiting=true;auto_op=0;auto_sequence=42;
 uint8_t cap[20]={0x23,0x70,0,42,0,2,0xff,0xff,0,0,1,1,7,9,0,0,0,0,0,0x21};
 m1_mcu_rx(cap);assert(details.supported && m1_state.mcu_sensor_details);
 static const uint8_t pmraw[32]={0x42,0x4d,0,0x1c,0,13,0,19,0,19,0,13,0,19,0,19,8,0x94,2,0x7f,0,0x72,0,0,0,0,0,0,0x97,0,3,0x37};
 details.frames[0]=(m1_sensor_frame_t){.length=32,.flags=3,.health=1,.received=true,.at=now,.age_ms=628};
 memcpy(details.frames[0].raw,pmraw,32);
 out=m1_sensor_details_json();assert(fixture_description(out));assert(fixture_bool(fixture_get(out,"available")));
 fixture_value *pm=fixture_get(out,"pm"),*counts=fixture_get(pm,"particles_per_0_1_l");
 assert(fixture_int(fixture_get(counts,"gt_0_3"))==2196);
 assert(fixture_int(fixture_get(counts,"gt_2_5"))==0);
 assert(fixture_int(fixture_get(fixture_get(pm,"atmospheric_ug_m3"),"pm1"))==13);fixture_release(out);
 now+=5372;out=m1_sensor_details_json();assert(!fixture_bool(fixture_get(out,"available")));assert(!fixture_get(out,"pm"));fixture_release(out);

 /* Real automatic scheduler performs metadata + four chunks + verification. */
 phase=10;details=(m1_details_t){.supported=true};auto_phase=6;auto_waiting=false;auto_due=manual_until=now;
 m1_state.mcu_extended=m1_state.mcu_version_received=true;
 for(unsigned turn=0;turn<6;turn++){
  m1_mcu_tick();assert(auto_waiting && auto_op==16);
  uint8_t reply[20]={0x23,0x70,16,auto_sequence,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x21};
  if(details.stage==1){unsigned n=32-details.offset;if(n>9)n=9;reply[6]=details.offset;reply[7]=n;reply[9]=5;memcpy(reply+10,pmraw+details.offset,n);}
  else {reply[6]=32;reply[8]=5;reply[13]=3;reply[14]=1;}
  m1_mcu_rx(reply);now+=10;
 }
 assert(details.frames[0].received && details.id==1 && auto_phase==6);
 assert(m1_details_valid(&details,now));
 cap[1]=0x17;m1_mcu_rx(cap);assert(!details.supported && !m1_state.mcu_sensor_details);
 /* Sensor alarms: capability-gated default OFF, separate write/read, independent
  * settings, flash rollback, CAS and UART error/malformed-response recovery. */
 appRestoreDefault_callback(&live,sizeof(live));memset(&m1_state,0,sizeof(m1_state));
 now=100000;phase=17;alarm_writes=alarm_reads=0;alarm_device_mask=7;
 auto_waiting=false;auto_due=manual_until=alarm_due=0;alarm_need_read=false;memset(&details,0,sizeof(details));
 m1_state.mcu_version_received=true;strcpy(m1_state.mcu_version,"0.9.3-test");
 m1_mcu_tick();assert(m1_state.mcu_alarm_schema==1 && !alarm_writes);
 m1_mcu_tick();assert(alarm_writes==1 && !alarm_reads && !m1_state.alarm_buzzer_received && alarm_device_mask==0);
 out=m1_alarm_json();assert(!fixture_bool(fixture_get(out,"buzzer_confirmed")));fixture_release(out);
 m1_mcu_tick();assert(alarm_reads==1 && m1_state.alarm_buzzer_received);
 out=m1_alarm_json();assert(fixture_description(out));
 assert(fixture_bool(fixture_get(out,"buzzer_confirmed")));
 assert(!strcmp(fixture_text(fixture_get(out,"buzzer_confirmation")),"mcu_readback"));fixture_release(out);
 const char *hcho_on="{\"sensor\":\"hcho\",\"yellow\":90,\"red\":240,\"revision\":0,\"buzzer_enabled\":true}";
 assert(!strcmp(command("alarm.set",hcho_on),"accepted"));m1_alarm_get(&a);
 assert(a.reserved[1]==1 && a.hcho[0]==90);live=flash;m1_alarm_get(&a);assert(a.reserved[1]==1);
 out=m1_alarm_json();assert(!fixture_bool(fixture_get(out,"buzzer_confirmed")));fixture_release(out);
 assert(!strcmp(command("alarm.set",hcho_on),"busy"));
 const char *pm25_on="{\"sensor\":\"pm25\",\"yellow\":40,\"red\":80,\"revision\":1,\"buzzer_enabled\":true}";
 preserved=live;fail_flash=true;assert(!strcmp(command("alarm.set",pm25_on),"storage_error"));fail_flash=false;
 assert(!memcmp(&live,&preserved,sizeof(live)));assert(!strcmp(command("alarm.set",pm25_on),"accepted"));
 assert(!strcmp(command("alarm.set","{\"sensor\":\"hcho\",\"yellow\":100,\"red\":300,\"revision\":2}"),"accepted"));
 m1_alarm_get(&a);assert(a.reserved[1]==3 && a.pm25[0]==40);
 assert(!strcmp(command("alarm.set","{\"sensor\":\"hcho\",\"yellow\":100,\"red\":300,\"revision\":3,\"buzzer_enabled\":false}"),"accepted"));
 m1_alarm_get(&a);assert(a.reserved[1]==2 && a.pm25[0]==40);
 for(unsigned fault=1;fault<=7;fault++){
  m1_state.alarm_buzzer_received=false;alarm_need_read=false;auto_waiting=false;auto_due=manual_until=now;alarm_due=now;
  alarm_io_fault=fault<4?fault:0;m1_mcu_tick();
  if(fault>=4){alarm_io_fault=fault;m1_mcu_tick();}
  out=m1_alarm_json();assert(fixture_description(out) && !fixture_bool(fixture_get(out,"buzzer_confirmed")));fixture_release(out);
  for(unsigned turn=0;turn<28;turn++){now+=250;m1_mcu_tick();}
  assert(m1_state.alarm_buzzer_received && alarm_device_mask==2);
  out=m1_alarm_json();assert(fixture_bool(fixture_get(out,"buzzer_confirmed")));fixture_release(out);
 }
 /* Old capability data never authorizes OP20, nor invents actual OFF state. */
 unsigned before_alarms=alarm_writes+alarm_reads;
 m1_state.mcu_alarm_schema=0;m1_state.alarm_buzzer_received=false;
 for(unsigned turn=0;turn<24;turn++){now+=250;m1_mcu_tick();}
 assert(alarm_writes+alarm_reads==before_alarms);
 out=m1_alarm_json();assert(fixture_description(out));
 assert(!fixture_bool(fixture_get(out,"buzzer_supported")));
 assert(!fixture_get(out,"mcu_hcho_buzzer_enabled"));
 assert(!strcmp(fixture_text(fixture_get(out,"buzzer_confirmation")),"requires_mcu_upgrade"));fixture_release(out);
 m1_state.mcu_alarm_schema=1;m1_state.alarm_buzzer_received=true;m1_state.alarm_buzzer_updated_ms=now-15000;
 out=m1_alarm_json();assert(!fixture_bool(fixture_get(out,"buzzer_confirmed")) && !fixture_get(out,"mcu_pm25_buzzer_enabled"));fixture_release(out);
 cap[1]=0x17;m1_mcu_rx(cap);assert(!m1_state.mcu_alarm_schema && !m1_state.alarm_buzzer_received && !m1_state.alarm_uart_sent);
 /* Physical reset works offline and retries rejection/storage/TX failures.
  * A duplicate must neither ACK uncommitted settings nor commit twice. */
 appRestoreDefault_callback(&live,sizeof(live));memset(&m1_state,0,sizeof(m1_state));
 strcpy(live.base.ssid,"old-network");strcpy(live.base.wifi_password,"old-key");
 strcpy(live.base.web_password,"old-web");strcpy(live.base.mqtt_host,"keep-broker");
 strcpy(live.base.mqtt_password,"keep-key");flash=live;
 now=0;step=queued=reset_rx=reset_acks=0;phase=18;reset_writes=writes;
 m1_state.ota_active=true;fail_flash=true;
 if(!setjmp(done))uart_worker(0);
 assert(reset_acks==2 && writes==reset_writes+2 && reset_rx==80);
 assert(m1_state.reboot_pending && m1_state.reboot_at==1300);
 assert(!m1_state.wifi_connected && !m1_state.mqtt_connected);
 /* A single event plus legacy state spans the receive work budget. */
 memset(&m1_state,0,sizeof(m1_state));m1_state.brightness_requested=4;
 now=0;step=queued=screen_rx=0;phase=19;button_seen=false;
 if(!setjmp(done))uart_worker(0);
 assert(screen_rx==40 && !m1_state.brightness_requested);
 assert(!m1_state.wifi_connected && !m1_state.mqtt_connected);
 /* Extended reset: exact schema/scope/token, rollback before ACK, one
  * durable write despite ACK loss, and no escalation by another request. */
 phase=20;
 for(unsigned kind=1;kind<=2;kind++){
  appRestoreDefault_callback(&live,sizeof(live));memset(&m1_state,0,sizeof(m1_state));
  strcpy(live.base.ssid,"old-network");strcpy(live.base.wifi_password,"old-key");
  strcpy(live.base.web_password,"old-web");strcpy(live.base.mqtt_host,"keep-broker");
  strcpy(live.base.mqtt_password,"keep-key");flash=live;preserved=live;
  reset_receipt_t receipt={0};reset_acks=0;reset_writes=writes;
  uint8_t request[20]={[0]=0x23,[1]=0x72,[2]=1,[3]=kind,[4]=255,[19]=0x21};
  request[2]=2;assert(reset_frame(&receipt,request));request[2]=1;
  request[3]=0;assert(reset_frame(&receipt,request));request[3]=3;assert(reset_frame(&receipt,request));request[3]=kind;
  for(unsigned i=5;i<19;i++){request[i]=1;assert(reset_frame(&receipt,request));request[i]=0;}
  assert(!reset_acks && writes==reset_writes && !receipt.committed);
  bool *busy[]={&m1_state.ota_active,&m1_state.mcu_updating,&m1_state.wifi_trial_active,&m1_state.reboot_pending};
  for(unsigned i=0;i<4;i++){
   *busy[i]=true;assert(reset_frame(&receipt,request));*busy[i]=false;
   assert(!reset_acks && !receipt.committed && writes==reset_writes);
  }
  fail_flash=true;assert(reset_frame(&receipt,request));fail_flash=false;
  assert(!reset_acks && !receipt.committed && !m1_state.reboot_pending && writes==reset_writes+1);
  assert(!memcmp(&live,&preserved,sizeof(live)));
  assert(reset_frame(&receipt,request) && receipt.committed && reset_acks==1 && writes==reset_writes+2);
  const uint8_t expected_ack[12]={[0]=0x23,[1]=0x72,[2]=1,[3]=kind,[4]=255,[11]=0x21};
  assert(!memcmp(reset_ack,expected_ack,12) && m1_state.reboot_pending);
  assert(!flash.base.ssid[0] && !flash.base.wifi_password[0] && !flash.base.web_password[0]);
  assert((kind==1 && !strcmp(flash.base.mqtt_host,"keep-broker")) || (kind==2 && !flash.base.mqtt_host[0]));
  assert(reset_frame(&receipt,request) && reset_acks==2 && writes==reset_writes+2);
  request[3]=3-kind;assert(reset_frame(&receipt,request));request[3]=kind;
  request[4]=0;assert(reset_frame(&receipt,request));
  assert(reset_acks==2 && writes==reset_writes+2);
 }
 puts("M1 production storage, alarms, reset ACK/retries and offline single click: OK");
}
'''.replace('PRODUCTION', production).replace('MIGRATION', migration)
        # Match the actual watchdog prototype included by the worker.
        harness = harness.replace('void watchdog_beat(int slot){(void)slot;}', '#include "common/watchdog.h"\nvoid watchdog_beat(watchdog_slot_t slot){(void)slot;}')
        run_host(harness)

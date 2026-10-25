"""Execute production M1 persistence, command routing and UART worker with faults."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def run_host(source):
    json_dir = Path(os.environ.get('PHICOMM_TEST_JSON_DIR', ROOT / 'build/a1-sdk/mico-os/libraries/utilities/json_c'))
    with tempfile.TemporaryDirectory() as directory:
        tmp = Path(directory)
        shutil.copytree(json_dir, tmp / 'json')
        for path in (tmp / 'json').glob('*.c'):
            path.write_text(path.read_text().replace('%ld', '%d'))
        (tmp / 'test.c').write_text(source)
        command = [os.environ.get('CC', 'cc'), '-std=gnu99', '-g', '-fsanitize=undefined,address',
                   '-I'+str(ROOT), '-I'+str(ROOT/'M1'), '-I'+str(ROOT/'tests/host'), '-I'+str(tmp/'json'),
                   '-Wno-pointer-sign', str(tmp/'test.c'), str(ROOT/'M1/protocol.c'),
                   *map(str, (tmp/'json').glob('*.c')), '-o', str(tmp/'test')]
        subprocess.run(command, check=True)
        subprocess.run([str(tmp/'test')], check=True)


class StockRuntimeTest(unittest.TestCase):
    def test_m1_storage_commands_and_uart(self):
        main = (ROOT / 'M1/main.c').read_text()
        production = main[main.index('mico_mutex_t m1_lock'):main.index('int application_start')]
        migration = main[main.index('    bool controller_migration'):main.index('    /* Normalize the retained')]
        harness = r'''
#include "M1/m1.h"
#include "M1/config_legacy.h"
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
static int migrate(void){MIGRATION return 0;}
static unsigned step,queued,version_queries,buzzer_on,buzzer_off,stop_ms;
static unsigned wake_beeps, beep_times[8], screen_rx;
static unsigned threshold_attempts, threshold_success, threshold_first, threshold_last;
static bool fail_threshold;
static uint8_t threshold_frame[12];
static jmp_buf done;
uint32_t mico_rtos_get_time(void){return now;}
static unsigned phase;
void mico_thread_msleep(unsigned ms){if(phase>=8){now+=ms;m1_mcu_tick();}}
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
 if(phase>=8 && phase!=10){
  uint8_t ack[20]={0x23,0x70,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x21};
  if(p[1]==0x70){ack[2]=p[2];ack[3]=p[3];
   if(p[2]==0){ack[5]=phase==11?1:2;ack[6]=0x3f;ack[7]=0xff;}
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
void m1_scd40_tick(void){
 step++;now+=100;
 if(phase==7){
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
json_object *m1_status_json(void){return json_object_new_object();}
json_object *feature_json(const char *s){(void)s;return NULL;}
int feature_set(const char *s,json_object *p,json_object **o){(void)s;(void)p;(void)o;return 400;}
json_object *service_parse(const void *p,size_t n){char *s=strndup(p,n);json_object *o=json_tokener_parse(s);free(s);return o;}
#define HOST_M1 1
json_object *m1_status_live_json(void){return json_object_new_object();}
int mico_rtos_create_thread(void *t,int p,const char *n,void (*f)(mico_thread_arg_t),unsigned stack,int arg){(void)t;(void)p;(void)n;(void)f;(void)stack;(void)arg;return 0;}
#include "common/service_product.inc"
static const char *command(const char *a,const char *params){json_object *p=json_tokener_parse(params);const char *code;json_object *o=service_product(a,p,&code);if(o)json_object_put(o);json_object_put(p);return code;}
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
  assert(wake_beeps==((phase==1 || phase==6)?3:1));
  assert((unsigned)(beep_times[0]-start)==100);
  if(wake_beeps==3)assert((unsigned)(beep_times[1]-start)==2100 && (unsigned)(beep_times[2]-start)==4100);
 }
 m1_state.ota_active=true;
 assert(!strcmp(command("wake_alarm.start","{}"),"busy"));
 assert(!strcmp(command("wake_alarm.stop","{}"),"accepted"));
 for(unsigned version=1;version<=5;version++){
  appRestoreDefault_callback(&live,sizeof(live));live.base.version=version;strcpy(live.base.ssid,"keep");
  live.features.magic=FEATURE_MAGIC;live.automation.magic=AUTOMATION_MAGIC;
  memset(live.reserved_controller_config,0xa5,sizeof(live.reserved_controller_config));
  if(version<3){m1_storage_v2_t old={0};old.base.magic=M1_CONFIG_MAGIC;old.base.version=version;strcpy(old.base.ssid,"keep");memcpy(&live,&old,sizeof(old));}
  assert(!migrate() && live.base.version==M1_CONFIG_VERSION);
  assert(!strcmp(live.base.ssid,"keep"));
  for(unsigned j=0;j<20;j++)assert(live.reserved_controller_config[j]==0);
 }
 live.base.version=5;fail_flash=true;assert(migrate()!=0);fail_flash=false;
 memset(live.reserved_controller_config,0xa5,20);live.base.version=M1_CONFIG_VERSION;unsigned before=writes;
 assert(!migrate() && writes==before);

 /* Restored stock threshold API: persistence, admission and UART-only proof. */
 appRestoreDefault_callback(&live,sizeof(live));memset(&m1_state,0,sizeof(m1_state));
 memset(live.reserved_controller_config,0xa5,20);
 m1_alarm_config_t blank;m1_alarm_get(&blank);assert(!blank.configured && !blank.revision);
 unsigned blank_attempts=threshold_attempts;alarm_sync();assert(threshold_attempts==blank_attempts);
 json_object *caps=service_product_capabilities();
 const char *caps_text=json_object_to_json_string(caps);assert(strstr(caps_text,"alarm.get") && strstr(caps_text,"alarm.set"));json_object_put(caps);
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
  "{\"sensor\":\"hcho\",\"yellow\":1,\"red\":2,\"revision\":0,\"other\":1}"
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
 json_object *out=m1_alarm_json();assert(json_object_get_boolean(json_object_object_get(out,"uart_sent")));
 assert(!strcmp(json_object_get_string(json_object_object_get(out,"confirmation")),"uart_send_only"));json_object_put(out);
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
 out=m1_co2_json();assert(json_object_get_boolean(json_object_object_get(out,"available")) && json_object_get_boolean(json_object_object_get(out,"mcu_data_valid")));json_object_put(out);
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
 assert(!migrate() && live.base.version==7 && live.co2.mode==0 && live.co2.interval==5);
 assert(!memcmp(keep_alarm,live.reserved_controller_config,20));
 puts("M1 production storage compatibility, UART version/buzzer deadline, wake alarm and restored threshold persistence/retry/queue pressure/uptime-wrap: OK");
}
'''.replace('PRODUCTION', production).replace('MIGRATION', migration)
        # Match the actual watchdog prototype included by the worker.
        harness = harness.replace('void watchdog_beat(int slot){(void)slot;}', '#include "common/watchdog.h"\nvoid watchdog_beat(watchdog_slot_t slot){(void)slot;}')
        run_host(harness)

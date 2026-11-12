"""Exercise the production accessory controller and command codec with fake GPIO."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from tests.mp_fixture_vectors import header
ROOT=Path(__file__).resolve().parents[1]
class FreshAirTest(unittest.TestCase):
 def test_controller(self):
  harness=r'''
#include "A1/a1.h"
#include "A1/storage.h"
#include "common/config_mp.inc"
#include "common/service_mp.h"
void service_cache_changed(unsigned mask){(void)mask;}
#define MICO_GPIO_9 9
#define MICO_GPIO_10 10
#define MICO_GPIO_19 19
#define MICO_PWM_1 1
#define MICO_PWM_3 3
#define OUTPUT_PUSH_PULL 1
#define INPUT_PULL_UP 2
#define IRQ_TRIGGER_FALLING_EDGE 1
static uint32_t now;static unsigned primary,ceiling=100,commits,fail_pwm;
static float measured_duty,ppm=2000;static bool sensor_valid=true;
static void (*edge)(void *);
mico_mutex_t a1_lock,a1_config_lock;
a1_state_t a1_state;
static app_storage_t storage;
a1_config_t *a1_config=&storage.base;
uint32_t mico_rtos_get_time(void){return now;}
void a1_control_changed(void){commits++;}
int MicoPwmStop(int pin){(void)pin;return 0;}
int MicoPwmInitialize(int pin,unsigned frequency,float duty){assert(pin==1 && frequency==25000);measured_duty=duty;return fail_pwm?-1:0;}
int MicoPwmStart(int pin){(void)pin;return 0;}
int MicoGpioOutputHigh(int pin){if(pin==9)measured_duty=100;return 0;}
int MicoGpioOutputLow(int pin){if(pin==9)measured_duty=0;return 0;}
int MicoGpioInitialize(int pin,int mode){(void)pin;(void)mode;return 0;}
int MicoGpioDisableIRQ(int pin){assert(pin==10);return 0;}
int MicoGpioEnableIRQ(int pin,int trigger,void (*callback)(void *),void *arg){assert(pin==10 && trigger==1);(void)arg;edge=callback;return 0;}
unsigned feature_primary_ceiling(void){return ceiling;}
bool feature_fresh_values(unsigned stat,unsigned metrics,float v[4],float *co2){(void)stat;(void)metrics;memset(v,0,4*sizeof(float));*co2=ppm;return sensor_valid;}
CODECS
STORAGE
#include "A1/fresh_air.c"
/* Human-readable vectors are encoded by Python before C compilation. */
#include "mp_fixture_vectors.inc"
static const char *cmd_value(const char *action,const ph_value *p){
 ph_writer writer;const char *code;assert(phw_init(&writer,PH_WIRE_MSGPACK));
 assert(fresh_mp(action,p,&writer,&code));ph_buf *result=phw_take(&writer);assert(result);ph_buf_free(result);return code;
}
static const char *cmd(const char *action,const char *body){
 for(unsigned i=0;i<sizeof(fixture_vectors)/sizeof(*fixture_vectors);i++)if(!strcmp(body,fixture_vectors[i].text)){
  ph_value p;assert(phv_parse(PH_WIRE_MSGPACK,fixture_vectors[i].bytes,fixture_vectors[i].length,&p));return cmd_value(action,&p);
 }
 assert(!"missing constant MP vector");return NULL;
}
static const char *independent_coupling(void){
 ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,NULL,false);phw_int(&w,"revision",storage.fresh_air.revision);phw_int(&w,"coupling",2);phw_end(&w);
 ph_buf *b=phw_take(&w);assert(b);ph_value p;assert(phv_parse(PH_WIRE_MSGPACK,b->buf,b->len,&p));const char *code=cmd_value("fresh_air.config.set",&p);ph_buf_free(b);return code;
}
static void tick(bool master,unsigned desired){unsigned next;now+=100;fresh_prepare(master,desired,primary,now,&next);primary=next;fresh_finish(primary,now);}
int main(void){
 fresh_defaults(&storage.fresh_air);assert(storage.fresh_air.mapping.coupling==2);storage.fresh_air.mapping.coupling=0;fresh_boot();fresh_init();assert(measured_duty==0 && edge);
 assert(!strcmp(cmd("fresh_air.set","{\"on\":true,\"speed\":100}"),"accepted"));
 for(unsigned i=0;i<15;i++)tick(true,40);
 assert(current.applied==40 && primary==40 && measured_duty==40);
 unsigned before=commits;
 const char *bad[]={"{\"revision\":2,\"pwm_min\":90,\"pwm_max\":10}","{\"revision\":2,\"a\":\"1\"}","{\"revision\":2,\"coupling\":256}","{\"revision\":2,\"active_low\":1}","{\"revision\":2,\"boost_ratio\":0}"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++)assert(!strcmp(cmd("fresh_air.config.set",bad[i]),"invalid_params"));
 assert(commits==before);
 assert(!strcmp(cmd("fresh_air.config.set","{\"revision\":1,\"a\":-1}"),"conflict"));
 assert(!strcmp(cmd("fresh_air.config.set","{\"revision\":2,\"coupling\":1,\"boost_ratio\":200,\"a\":-0.5,\"b\":100,\"active_low\":true,\"pwm_min\":10,\"pwm_max\":90}"),"accepted"));
 ceiling=70;for(unsigned i=0;i<15;i++)tick(true,0);
 assert(primary==70 && current.applied==35 && measured_duty==82.5f);
 tick(false,0);assert(primary==0 && current.applied==0 && measured_duty==100);
 assert(storage.fresh_air.on && storage.fresh_air.speed==100);
 assert(!strcmp(cmd("fresh_air.linkage.set","{\"revision\":3,\"min_speed\":20,\"max_speed\":90,\"fallback_speed\":0,\"low_co2\":800,\"high_co2\":2000}"),"accepted"));
 assert(!strcmp(cmd("fresh_air.linkage.set","{\"revision\":4,\"high_co2\":100}"),"invalid_params"));
 assert(!strcmp(cmd("fresh_air.set","{\"mode\":\"auto\"}"),"accepted"));
 ceiling=100;for(unsigned i=0;i<15;i++)tick(true,0);
 assert(current.target==90 && primary==100 && current.applied==50);
 sensor_valid=false;now+=5000;tick(true,0);assert(current.target==20 && current.fallback);
 assert(!strcmp(cmd("fresh_air.set","{\"mode\":\"manual\",\"speed\":70}"),"accepted"));
 tick(true,0);assert(current.applied>0); /* Control edits must not pulse the fan off. */
 /* Snapshot reads must not advance controller state. */
 feature_pid_t saved=controller;fresh_snapshot_t view;for(unsigned i=0;i<50;i++)fresh_snapshot(&view);assert(!memcmp(&saved,&controller,sizeof(saved)));
 for(unsigned i=0;i<120;i++)edge(NULL);now+=1000;tick(true,0);assert(current.pulses==120 && current.rpm>0);
 sample_at=now;previous_pulses=pulses;config.mapping.pulses_per_rev=2;
 for(unsigned i=0;i<100;i++)edge(NULL);now+=1000;mico_rtos_lock_mutex(&output_lock);fresh_finish(primary,now);assert(current.rpm==3000);
 config.mapping.pulses_per_rev=4;for(unsigned i=0;i<100;i++)edge(NULL);now+=1000;mico_rtos_lock_mutex(&output_lock);fresh_finish(primary,now);assert(current.rpm==1500);
 config.mapping.pulses_per_rev=storage.fresh_air.mapping.pulses_per_rev;
 assert(!strcmp(cmd("fresh_air.set","{\"speed\":10}"),"accepted"));
 fail_pwm=1;tick(true,0);assert(current.applied==0 && !current.ready && measured_duty==100);fail_pwm=0;
 /* A saturated coupling must not accumulate upward PID integral. */
 storage.fresh_air.automatic=1;storage.fresh_air.linkage.mode=2;
 storage.fresh_air.linkage.min_speed=0;storage.fresh_air.linkage.max_speed=100;
 storage.fresh_air.linkage.kp=0;storage.fresh_air.linkage.ki=1;
 storage.fresh_air.co2.target_co2=1000;config=storage.fresh_air;
 sensor_valid=true;ppm=2000;primary=100;current.applied=50;published.reason=2;
 controller.initialized=true;controller.integral=70;controller.last_ms=now-5000;control_at=now-5000;
 tick(true,0);assert(controller.integral==70 && current.applied==50);
 assert(!strcmp(independent_coupling(),"accepted"));
 assert(!strcmp(cmd("fresh_air.set","{\"mode\":\"manual\",\"speed\":100}"),"accepted"));
 for(unsigned i=0;i<15;i++)tick(true,0);assert(primary==0 && current.applied==100);
 tick(false,0);assert(!primary && !current.applied);
 fresh_shutdown();for(unsigned i=0;i<10;i++)tick(true,100);assert(!primary && !current.applied && measured_duty==100);
 a1_state.reboot_pending=true;assert(!strcmp(cmd("fresh_air.set","{\"on\":false}"),"busy"));
 puts("Fresh controller: commands, revision, mapping, sensor fallback, feedback, shutdown: PASS");
}
'''
  runtime=(ROOT/'common/runtime.c').read_text();codecs=runtime[runtime.index('#include "fresh_config_mp.inc"'):runtime.rindex('#endif')]
  main=(ROOT/'A1/main.c').read_text();storage=main[main.index('void fresh_config_get('):main.rindex('#endif')]
  harness=harness.replace('CODECS',codecs).replace('STORAGE',storage)
  with tempfile.TemporaryDirectory() as temp:
   temp=Path(temp)
   (temp/'mp_fixture_vectors.inc').write_text(header([harness],{}))
   source=temp/'test.c';source.write_text(harness);out=temp/'test'
   command=['cc','-std=gnu99','-ffunction-sections','-Wl,--gc-sections','-DA1_FRESH_AIR','-DFEATURE_DEVICE_A1','-I'+str(ROOT),'-I'+str(ROOT/'A1'),'-I'+str(ROOT/'common'),'-I'+str(ROOT/'tests/host'),'-I'+str(temp),str(source),str(ROOT/'A1/fresh_air_defaults.c'),str(ROOT/'common/engine.c'),str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c'),'-lm','-o',str(out)]
   if os.environ.get('SANITIZE')=='1':command+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
   subprocess.run(command,check=True)
   subprocess.run([out],check=True)

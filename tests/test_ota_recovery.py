"""Production OTA recovery and runtime start functions under scheduler faults."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from tests.test_http_json_budget import function
ROOT=Path(__file__).resolve().parents[1]

class OtaRecovery(unittest.TestCase):
 def test_production_recovery(self):
  mode=(ROOT/'common/ota/mode.c').read_text()
  mode=mode[mode.index('static mico_mutex_t job_lock;'):mode.index('void ota_job_status_mp(')]
  runtime=(ROOT/'common/runtime.c').read_text()
  runtime='\n'.join(function(runtime,s) for s in ('static bool feature_worker_should_stop(', 'OSStatus feature_resume_step(', 'OSStatus feature_resume('))
  runtime+='\n'+function((ROOT/'common/runtime.c').read_text(),'bool feature_running(')+'\n'+function((ROOT/'common/runtime.c').read_text(),'bool feature_pause(').replace('bool feature_pause(', 'static bool actual_feature_pause(')
  fault=(ROOT/'tests/http_core/fault.c').read_text().replace('static void reset(void)', 'static void core_reset(void)').replace('int main(void)', 'int unused_fault_main(void)').replace('reset();','core_reset();')
  fault=fault.replace('tick+=ms;', 'tick+=ms;at=tick;').replace('releases++;if(sent)', 'releases++;if(claimed&&stopped)ota_job_release();if(sent)').replace('static unsigned admission(void){return desired;}', 'static unsigned admission(void){ota_job_poll();return desired;}')
  fixture=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define OTA_MODEL_A1 1
#define kNoErr 0
#define kNoMemoryErr -2
#define kInProgressErr -3
#define MICO_APPLICATION_PRIORITY 1
#define FEATURE_WORKER_STACK_SIZE 12288
typedef int OSStatus;typedef unsigned mico_mutex_t;typedef unsigned mico_thread_arg_t;
static bool worker_running,worker_stop,worker_exiting,worker_ready;
static unsigned runtime_lock,at,creates,sleeps,enable_calls;
static int create_rc;static bool instant_ready,reenter_poll,watch_active;
static struct {unsigned free_memory;} memory={40000};
#define MicoGetMemoryInfo() (&memory)
static void mico_rtos_lock_mutex(unsigned*p){assert(!*p);*p=1;}
static void mico_rtos_unlock_mutex(unsigned*p){assert(*p);*p=0;}
static int mico_rtos_init_mutex(unsigned*p){*p=0;return 0;}
static unsigned mico_rtos_get_time(void){return at;}
static unsigned now_ms(void){return at;}
static void mico_thread_msleep(unsigned ms){sleeps++;at+=ms;assert(sleeps<1000);}
static void lock(void){mico_rtos_lock_mutex(&runtime_lock);}
static void unlock(void){mico_rtos_unlock_mutex(&runtime_lock);}
static void watchdog_binding_enable(bool on){watch_active=on;enable_calls++;}
static void feature_worker(mico_thread_arg_t arg){(void)arg;}
static void ota_job_poll(void);
static int mico_rtos_create_thread(void*a,int p,const char*n,void(*f)(mico_thread_arg_t),unsigned size,int arg){
 assert(!a&&p==1&&!strcmp(n,"binding")&&f==feature_worker&&size==12288&&!arg);
 assert(!runtime_lock);creates++;
 if(reenter_poll)ota_job_poll(); /* Simulate the other owner while creation is pending. */
 if(!create_rc&&instant_ready)worker_ready=true;
 return create_rc;
}
static bool feature_pause(unsigned timeout){assert(timeout==5000);return true;}
static void service_event(const char*a,const char*b,const char*c){(void)a;(void)b;(void)c;}
/* RUNTIME_FUNCTIONS */
/* MODE_FUNCTIONS */
/* CORE_FAULT_BOUNDARY */
static void reset(void){
 claimed=stopped=cancelled=committing=restoring=recovery_busy=false;
 worker_running=worker_stop=worker_exiting=worker_ready=false;
 creates=sleeps=enable_calls=at=0;create_rc=0;instant_ready=reenter_poll=false;
 ota_job_init();
}
static void reserve_and_stop(void){assert(ota_job_claim());assert(ota_job_enter());assert(stopped&&claimed);}
int main(void){
 reset();watch_active=true;worker_running=worker_ready=true;
 assert(!feature_worker_should_stop()&&watch_active&&!worker_exiting);
 worker_stop=true;assert(watch_active); /* Request alone cannot mask a stuck worker. */
 assert(feature_worker_should_stop()&&worker_exiting&&!watch_active);
 assert(feature_resume_step()==kInProgressErr&&!watch_active&&!creates);
 assert(!actual_feature_pause(40)&&!watch_active&&worker_exiting);
 reset();watch_active=true;worker_running=worker_ready=true;
 assert(!actual_feature_pause(40)&&watch_active&&!worker_stop&&!worker_exiting);
 reset();reserve_and_stop();create_rc=-2;ota_job_release();
 assert(!creates&&!sleeps&&claimed&&restoring&&!strcmp(phase,"restoring"));
 for(unsigned round=0;round<10;round++){
  ota_job_poll();assert(creates==round+1&&!worker_running&&claimed&&restoring&&!recovery_busy&&!watch_active);
  for(unsigned n=0;n<100;n++){ota_job_poll();ota_job_release();assert(ota_job_active());}
  assert(creates==round+1&&!sleeps);at+=1000;
 }
 assert(!ota_job_claim()); /* A failed restore cannot release OTA exclusion. */
 create_rc=0;reenter_poll=true;ota_job_poll();assert(creates==11&&worker_running&&!worker_ready&&claimed&&watch_active);
 at+=1000;ota_job_poll();assert(creates==11&&claimed); /* Late ready: never duplicate. */
 worker_ready=true;at+=1000;ota_job_poll();assert(!claimed&&!restoring&&!stopped&&!sleeps);
 assert(ota_job_claim());unsigned token=generation;
 /* Stale recovery polls cannot run a worker or clear a newly reserved job. */
 ota_job_poll();assert(claimed&&generation==token&&creates==11);
 assert(ota_job_enter());ota_job_release();ota_job_release();at+=1000;ota_job_poll();assert(!claimed);
 reset();reserve_and_stop();worker_running=worker_exiting=true;ota_job_release();
 for(unsigned n=0;n<20;n++){ota_job_poll();at+=1000;assert(claimed&&worker_exiting&&!creates&&!sleeps);}
 worker_running=worker_exiting=false;instant_ready=true;ota_job_poll();assert(!claimed&&creates==1);
 reset();worker_running=worker_exiting=true;unsigned start=at;
 assert(feature_resume()==kNoMemoryErr&&at-start==5000&&!creates);
 reset();assert(feature_resume_step()==kInProgressErr&&creates==1&&!sleeps);
 assert(feature_resume_step()==kInProgressErr&&creates==1);worker_ready=true;
 assert(feature_resume_step()==kNoErr&&creates==1);
 reset();create_rc=-2;assert(feature_resume_step()==-2&&!worker_running&&creates==1&&!sleeps);
 create_rc=0;instant_ready=true;assert(feature_resume_step()==0&&creates==2);
 reset();at=UINT32_MAX-500;reserve_and_stop();create_rc=-2;ota_job_release();ota_job_poll();
 at+=999;ota_job_poll();assert(creates==1);at++;instant_ready=true;create_rc=0;ota_job_poll();assert(creates==2&&!claimed);
 reset();reserve_and_stop();create_rc=-2;core_reset();
 connect_one(1,"POST /body HTTP/1.1\r\nContent-Length:3\r\n\r\nabc");body_fail=true;
 run(100);assert(s.running&&releases==1&&claimed&&restoring&&!sleeps);
 connect_one(2,"GET / HTTP/1.1\r\n\r\n");run(100);
 assert(s.running&&releases==2&&strstr(output[2],"payload")&&claimed&&restoring&&!sleeps);
 create_rc=0;instant_ready=true;run(100);assert(!claimed&&!restoring&&!sleeps);ph_http_server_close(&s);
 puts("production OTA recovery: persistent OOM/exiting bounded; throttle, reentrant poll, late ready, retry and wrap PASS");
}
'''.replace('/* RUNTIME_FUNCTIONS */',runtime).replace('/* MODE_FUNCTIONS */',mode).replace('/* CORE_FAULT_BOUNDARY */',fault)
  # Production poll has external linkage.
  fixture=fixture.replace('static void ota_job_poll(void);','void ota_job_poll(void);')
  with tempfile.TemporaryDirectory() as temp:
   source=Path(temp)/'probe.c';source.write_text(fixture);binary=Path(temp)/'probe'
   subprocess.run([os.environ.get('CC','cc'),'-std=c99','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(ROOT/'common/httpd'),str(source),str(ROOT/'common/httpd/core.c'),str(ROOT/'common/httpd/parser.c'),'-o',str(binary)],check=True)
   subprocess.run([str(binary)],check=True,timeout=10)

if __name__=='__main__':unittest.main()

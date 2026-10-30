"""Actual TC1 start/pause and OTA restore state machine under scheduler faults."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tests.test_native_udp_runtime import function
ROOT=Path(__file__).resolve().parents[1]
class TC1LanLifecycle(unittest.TestCase):
 def test_nonblocking_start_bounded_pause_and_ota_restore(self):
  lan=(ROOT/'TC1/lan.c').read_text();mode=(ROOT/'common/ota/mode.c').read_text();mode=mode[mode.index('static mico_mutex_t job_lock;'):mode.index('void ota_job_status_mp(')]
  source=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#define OTA_MODEL_TC1 1
#define kNoErr 0
#define kNoMemoryErr -2
#define kUnknownErr -3
#define kInProgressErr -4
#define MICO_APPLICATION_PRIORITY 7
#define TC1_LAN_STACK_SIZE 12288u
typedef int OSStatus;typedef unsigned mico_mutex_t;typedef unsigned mico_thread_arg_t;
static char tc1_lan_id[13],str_mac[]="D0BAE4636D3D";
static mico_mutex_t tc1_lan_lock;
static bool tc1_lan_lock_ready,tc1_lan_initialized,tc1_lan_running,tc1_lan_ready,tc1_lan_stop_requested;
static uint32_t tc1_lan_boot,at;static unsigned creates,mutex_inits,sleeps;static int random_rc,create_rc;static bool finish_on_sleep;
static struct {unsigned free_memory;} memory={40000};
#define MicoGetMemoryInfo() (&memory)
static int mico_rtos_init_mutex(unsigned*p){mutex_inits++;*p=0;return 0;}
static void mico_rtos_lock_mutex(unsigned*p){assert(!*p);*p=1;}
static void mico_rtos_unlock_mutex(unsigned*p){assert(*p);*p=0;}
static uint32_t mico_rtos_get_time(void){return at;}
static int MicoRandomNumberRead(uint8_t*p,unsigned n){memset(p,0,n);return random_rc;}
static void mico_thread_msleep(unsigned n){at+=n;sleeps++;if(finish_on_sleep)tc1_lan_running=tc1_lan_ready=false;assert(sleeps<1000);}
static void tc1_lan_worker(mico_thread_arg_t arg){(void)arg;}
static int mico_rtos_create_thread(void*h,int priority,const char*name,void(*run)(mico_thread_arg_t),unsigned stack,int arg){assert(!h&&priority==8&&!strcmp(name,"lan_business")&&run==tc1_lan_worker&&stack==12288&&!arg);creates++;return create_rc;}
static void service_event(const char*a,const char*b,const char*c){(void)a;(void)b;(void)c;}
/* LAN */
/* MODE */
int main(void){
 random_rc=-1;assert(tc1_lan_start()==kUnknownErr&&mutex_inits==1);assert(tc1_lan_start()==kUnknownErr&&mutex_inits==1);
 random_rc=0;create_rc=-2;assert(tc1_lan_start()==-2&&!tc1_lan_running&&creates==1);assert(!strcmp(tc1_lan_id,"d0bae4636d3d")&&tc1_lan_boot==1);
 create_rc=0;assert(tc1_lan_start()==kInProgressErr&&tc1_lan_running&&creates==2);assert(tc1_lan_start()==kInProgressErr&&creates==2);
 tc1_lan_ready=true;assert(tc1_lan_start()==kNoErr&&creates==2);
 assert(!tc1_lan_pause(40)&&at==40&&tc1_lan_stop_requested);assert(tc1_lan_start()==kInProgressErr&&creates==2);
 finish_on_sleep=true;assert(tc1_lan_pause(40)&&!tc1_lan_running);finish_on_sleep=false;
 ota_job_init();assert(ota_job_claim());assert(ota_job_enter()&&stopped);ota_job_release();assert(claimed&&restoring);
 create_rc=-2;ota_job_poll();assert(claimed&&restoring&&!tc1_lan_running);unsigned before=creates;
 for(unsigned i=0;i<10;i++){ota_job_poll();ota_job_release();assert(creates==before&&claimed);}
 at+=1000;create_rc=0;ota_job_poll();assert(creates==before+1&&claimed&&tc1_lan_running&&!tc1_lan_ready);
 at+=1000;ota_job_poll();assert(claimed&&creates==before+1);tc1_lan_ready=true;at+=1000;ota_job_poll();assert(!claimed&&!restoring&&creates==before+1);
 puts("actual TC1 start/pause + OTA: RNG/create retry, no duplicate worker, bounded stop, exclusion until ready PASS");return 0;
}
'''.replace('/* LAN */','\n'.join(function(lan,s) for s in ('OSStatus tc1_lan_start(','bool tc1_lan_pause('))).replace('/* MODE */',mode)
  with tempfile.TemporaryDirectory() as d:
   t=Path(d);(t/'case.c').write_text(source);cmd=['cc','-std=c99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',str(t/'case.c'),'-o',str(t/'case')]
   built=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(built.returncode,0,built.stderr)
   ran=subprocess.run([str(t/'case')],capture_output=True,text=True);self.assertEqual(ran.returncode,0,ran.stderr);print(ran.stdout.strip())

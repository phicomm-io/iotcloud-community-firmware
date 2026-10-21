"""Real TC1 guard + OTA/config reservation, with competing snapshot/writer threads."""
from pathlib import Path
import os, subprocess, tempfile, unittest
ROOT=Path(__file__).resolve().parents[1]
def function(source,name):
 start=source.index(name+'(');start=source.rfind('\n',0,start)+1
 end=source.index('{',start);depth=1;end+=1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
class TC1StatusGuard(unittest.TestCase):
 def test_real_reservation_and_snapshot_domain(self):
  source=(ROOT/'TC1/ota_server/signed_adapter.c').read_text()
  functions='\n'.join(function(source,n) for n in ('tc1_config_reserve','tc1_config_release','tc1_config_changing'))
  with tempfile.TemporaryDirectory() as tmp:
   d=Path(tmp)
   (d/'mico.h').write_text('''#pragma once
#include <stdbool.h>
#include <pthread.h>
typedef pthread_mutex_t mico_mutex_t;
#define kNoErr 0
extern int init_failed;
static int mico_rtos_init_mutex(mico_mutex_t *m){return init_failed?-1:pthread_mutex_init(m,0);}
static void mico_rtos_lock_mutex(mico_mutex_t *m){pthread_mutex_lock(m);}
static void mico_rtos_unlock_mutex(mico_mutex_t *m){pthread_mutex_unlock(m);}
''')
   (d/'test.c').write_text('''#include "mico.h"
#include "status_guard.h"
#include <assert.h>
#include <stdatomic.h>
#include <sched.h>
int init_failed;
static mico_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static bool active,config_busy,initialized=true;
'''+functions+'''
static atomic_bool entered,finished;
static void *writer(void *unused){(void)unused;entered=true;assert(tc1_config_reserve());finished=true;return 0;}
int main(void){
 assert(!tc1_status_begin());assert(!tc1_config_reserve());
 init_failed=1;assert(!tc1_status_guard_init());assert(!tc1_status_begin());
 init_failed=0;assert(tc1_status_guard_init());assert(tc1_status_begin());
 pthread_t t;assert(!pthread_create(&t,0,writer,0));while(!entered)sched_yield();
 for(unsigned i=0;i<10000;i++)sched_yield();assert(!finished);
 tc1_status_unlock();pthread_join(t,0);assert(finished);
 /* All groups reject throughout SDK restore, Flash and complete rollback. */
 assert(!tc1_status_begin());assert(!tc1_config_reserve());
 tc1_config_release();assert(tc1_status_begin());tc1_status_unlock();
 /* OTA reservation alone does not invalidate otherwise coherent live state. */
 active=true;assert(!tc1_config_reserve());assert(tc1_status_begin());tc1_status_unlock();active=false;
 for(unsigned i=0;i<10000;i++){assert(tc1_config_reserve());assert(!tc1_status_begin());tc1_config_release();assert(tc1_status_begin());tc1_status_unlock();}
 return 0;
}
''')
   cmd=[os.environ.get('CC','cc'),'-std=gnu11','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-pthread','-I'+str(d),'-I'+str(ROOT/'TC1'),str(d/'test.c'),str(ROOT/'TC1/status_guard.c'),'-o',str(d/'test')]
   subprocess.run(cmd,check=True);subprocess.run([d/'test'],check=True)
if __name__=='__main__':unittest.main()

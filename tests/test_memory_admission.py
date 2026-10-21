"""Run actual no-heap admission/wait/HTTP paths with a changing heap budget."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from tests.test_tc1_sdk_migration import function
ROOT = Path(__file__).resolve().parents[1]

class MemoryAdmission(unittest.TestCase):

    def run_c(self, source):
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory)/'test.c'; p.write_text("#define HTTP_TRACE(phase,fd,detail) ((void)(detail))\n"+source)
            cmd=[os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(ROOT),str(p),'-o',str(p.with_suffix(''))]
            if os.environ.get('SANITIZE')=='1': cmd+=['-g','-fno-omit-frame-pointer','-fsanitize=address,undefined']
            subprocess.run(cmd,check=True);subprocess.run([str(p.with_suffix(''))],check=True)

    def test_budget_overlap_boundaries_and_wait_recovery(self):
        self.run_c(r'''
#include <assert.h>
#include <stdio.h>
#include "common/memory_budget.h"
static service_memory_state_t state;
static uint32_t now,free_bytes,recover_at;
static unsigned sleeps;
static unsigned service_memory_claim(size_t n){return memory_budget_claim(&state,free_bytes,n);}
static void service_memory_waiting(bool enter){if(enter)state.waiting++;else{assert(state.waiting);state.waiting--;}}
static uint32_t mico_rtos_get_time(void){return now;}
static void mico_thread_msleep(unsigned ms){assert(ms==25 && state.waiting==1);sleeps++;now+=ms;if(sleeps==recover_at)free_bytes=40000;}
#include "common/memory_wait.h"
int main(void){
 state.minimum_free=UINT32_MAX;
 assert(!memory_budget_claim(&state,0,0));assert(!memory_budget_claim(&state,16895,0));
 unsigned a=memory_budget_claim(&state,16896,0);assert(a==512 && state.active==1);
 assert(!memory_budget_claim(&state,17407,0)); /* competing builders share the budget */
 unsigned b=memory_budget_claim(&state,17408,0);assert(b && state.active==2);
 memory_budget_release(&state,a);memory_budget_release(&state,b);assert(!state.active && !state.reserved);
 assert(!memory_budget_claim(&state,UINT32_MAX,SIZE_MAX));
 a=memory_budget_claim(&state,25088,8192);assert(a==8704);memory_budget_release(&state,a);
 now=UINT32_MAX-50;free_bytes=1000;recover_at=4;
 a=service_memory_wait(0);assert(a && sleeps==4 && now==49 && !state.waiting);memory_budget_release(&state,a);
 sleeps=0;recover_at=UINT32_MAX;free_bytes=1000;now=UINT32_MAX-50;
 assert(!service_memory_wait(0) && sleeps==80 && !state.waiting);
 sleeps=0;assert(!service_memory_wait(1025) && !sleeps && !state.waiting);
 assert(!service_memory_wait(8192) && !sleeps && !state.waiting);
 free_bytes=25088;a=service_memory_wait(8192);assert(a==8704 && !sleeps);memory_budget_release(&state,a);
 free_bytes=1000;assert(!service_memory_wait(1024) && sleeps==80 && !state.waiting);
 assert(!state.active && !state.reserved && state.minimum_free==0);
 puts("Shared reservations: floor/overlap/overflow; wait keeps descriptor, resumes, times out across tick wrap: OK");
}
''')


    def test_fixed_busy_identifiers(self):
        self.run_c(r'''
#include <assert.h>
#include <stdio.h>
#include "common/busy_reply.h"
static void check(const char *s,const char *id){char out[SERVICE_BUSY_BYTES];service_busy_wire(s,s?strlen(s):0,out);assert(strstr(out,"\"code\":\"busy\""));if(id)assert(strstr(out,id));else assert(!strstr(out,"request_id"));}
int main(void){
 check(NULL,NULL);check("",NULL);check("{}",NULL);
 check("{\"x\":[{\"request_id\":\"nested\"}],\"request_id\":\"real-1\"}","real-1");
 check("{\"request_id\":\"escaped\\u002d1\"}","escaped-1");
 check("{\"request_id\":null}",NULL);check("{\"request_id\":\"bad\\u0000tail\"}",NULL);
 check("{\"request_id\":\"bad\\\"tail\"}",NULL);check("{\"request_id\":\"valid\"}trailing",NULL);
 check("{\"request_id\":\"valid\",",NULL);
 char p[512];snprintf(p,sizeof(p),"{\"request_id\":\"%080d\"}",1);check(p,NULL);
 /* Truncation and arbitrary escaped bytes cannot overflow fixed buffers. */
 for(unsigned c=1;c<256;c++){snprintf(p,sizeof(p),"{\"request_id\":\"\\%c\"}",c);check(p,NULL);}
 puts("Fixed overload replies: top-level request ID, escaping, truncation and oversized ID bounded: OK");
}
''')

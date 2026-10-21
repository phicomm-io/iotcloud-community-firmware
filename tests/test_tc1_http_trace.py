"""Exercise retained numeric tracing across simulated warm/cold boots."""
from pathlib import Path
import unittest
from tests import test_memory_admission as admission
ROOT=Path(__file__).resolve().parents[1]

class Trace(unittest.TestCase):
 def test_warm_reset_invalid_record_build_change_and_bounded_format(self):
  helper=admission.MemoryAdmission()
  hooks=helper.sdk_http_source('http_trace_hooks.h').replace('#pragma once','')
  source=(ROOT/'TC1/http_trace.c').read_text()
  source='\n'.join(line for line in source.splitlines() if not line.startswith('#include "'))
  helper.run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#undef HTTP_TRACE
#define PHICOMM_HTTP_TRACE 1
#define FIRMWARE_VERSION "trace-test"
typedef struct {int free_memory;} micoMemInfo_t;
static micoMemInfo_t heap={40000};static uint32_t now;
static const micoMemInfo_t *MicoGetMemoryInfo(void){errno=EIO;return &heap;}
static uint32_t mico_rtos_get_time(void){return now++;}
HOOKS
SOURCE
static void warm_boot(void){memset(&previous,0,sizeof(previous));initialized=false;sampled_free=0;}
int main(void){
 char body[1536];memset((void*)&retained,0xa5,sizeof(retained));
 assert(tc1_http_trace_format(body,sizeof(body)) && strstr(body,"\"previous_valid\":false"));
 for(unsigned i=0;i<10000;i++){errno=EAGAIN;phicomm_http_trace(HTTP_TRACE_RECV_RETURN,7,1);assert(errno==EAGAIN);}
 heap.free_memory=25000;phicomm_http_trace(HTTP_TRACE_CLOSE_ENTER,7,0);
 uint32_t last=retained.sequence;warm_boot();assert(tc1_http_trace_format(body,sizeof(body)));
 assert(strstr(body,"\"previous_valid\":true") && strstr(body,"\"minimum_sampled_free\":25000"));
 assert(previous.sequence==last && previous.records[(last-1)%TRACE_SLOTS].phase==HTTP_TRACE_CLOSE_ENTER);
 for(size_t n=0;n<10;n++)assert(!tc1_http_trace_format(body,n));
 /* A partial record can never be mistaken for a committed operation. */
 phicomm_http_trace(HTTP_TRACE_SEND_ENTER,9,512);retained.records[(retained.sequence-1)%TRACE_SLOTS].seal=0;
 warm_boot();assert(tc1_http_trace_format(body,sizeof(body)));assert(!strstr(body,"\"fd\":9"));
 retained.build^=1;warm_boot();assert(tc1_http_trace_format(body,sizeof(body))&&strstr(body,"\"previous_valid\":false"));
 /* Format maximum fields without overflow; corrupted sequence remains bounded. */
 previous.magic=TRACE_MAGIC;previous.sequence=UINT32_MAX;previous.minimum_free=UINT32_MAX;
 for(unsigned i=0;i<TRACE_SLOTS;i++){
  uint32_t seq=UINT32_MAX-i;trace_record_t *r=&previous.records[(seq-1)%TRACE_SLOTS];
  *r=(trace_record_t){seq,HTTP_TRACE_REPLY_RETURN,UINT32_MAX,INT32_MIN,INT32_MIN,UINT32_MAX,~seq};
 }
 assert(tc1_http_trace_format(body,sizeof(body)) && strlen(body)<sizeof(body));
 assert(sizeof(retained)<=512);
 puts("TC1 retained trace: 10000 events, warm/cold/build change, torn commit, errno preserved, max formatting and memory budget: OK");
}
'''.replace('HOOKS',hooks).replace('SOURCE',source))

if __name__=='__main__':unittest.main()

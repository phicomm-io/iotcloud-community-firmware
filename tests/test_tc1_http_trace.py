"""Exercise retained numeric tracing across simulated warm/cold boots."""
from pathlib import Path
import unittest
from tests import test_sensor_producers_mp as producers
ROOT=Path(__file__).resolve().parents[1]

class Trace(unittest.TestCase):
 def test_warm_reset_invalid_record_build_change_and_bounded_format(self):
  helper=producers.SensorProducers()
  hooks=(ROOT/'common/httpd/http_trace_hooks.h').read_text().replace('#pragma once','')
  source=(ROOT/'TC1/http_trace.c').read_text()
  source='\n'.join(line for line in source.splitlines() if not line.startswith('#include "'))
  helper.compile_run(r'''
#include "common/typed_wire.h"
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
static bool decode(char *body,size_t capacity,ph_value *data){
 ph_writer w;ph_buf b;ph_value root;phw_init_fixed(&w,PH_WIRE_MSGPACK,&b,body,capacity);tc1_http_trace_mp(&w);
 size_t n=phw_finish_fixed(&w);return n&&phv_parse(PH_WIRE_MSGPACK,body,n,&root)&&phv_get(&root,"data",data);
}
static bool valid_data(const ph_value *data){ph_value v;bool b;assert(phv_get(data,"previous_valid",&v)&&phv_bool(&v,&b));return b;}
static void warm_boot(void){memset(&previous,0,sizeof(previous));initialized=false;sampled_free=0;}
int main(void){
 char body[1536];ph_value data,v,records;int64_t number;memset((void*)&retained,0xa5,sizeof(retained));
 assert(decode(body,sizeof(body),&data)&&!valid_data(&data));
 for(unsigned i=0;i<10000;i++){errno=EAGAIN;phicomm_http_trace(HTTP_TRACE_RECV_RETURN,7,1);assert(errno==EAGAIN);}
 heap.free_memory=25000;phicomm_http_trace(HTTP_TRACE_CLOSE_ENTER,7,0);
 uint32_t last=retained.sequence;warm_boot();assert(decode(body,sizeof(body),&data));
 assert(valid_data(&data)&&phv_get(&data,"minimum_sampled_free",&v)&&phv_int(&v,&number)&&number==25000);
 assert(previous.sequence==last && previous.records[(last-1)%TRACE_SLOTS].phase==HTTP_TRACE_CLOSE_ENTER);
 for(size_t n=0;n<10;n++)assert(!decode(body,n,&data));
 /* A partial record can never be mistaken for a committed operation. */
 phicomm_http_trace(HTTP_TRACE_SEND_ENTER,9,512);retained.records[(retained.sequence-1)%TRACE_SLOTS].seal=0;
 warm_boot();assert(decode(body,sizeof(body),&data));assert(phv_get(&data,"records",&records));for(unsigned i=0;i<records.count;i++){assert(phv_at(&records,i,&v));ph_value fd;assert(phv_get(&v,"fd",&fd)&&phv_int(&fd,&number)&&number!=9);}
 retained.build^=1;warm_boot();assert(decode(body,sizeof(body),&data)&&!valid_data(&data));
 /* Format maximum fields without overflow; corrupted sequence remains bounded. */
 previous.magic=TRACE_MAGIC;previous.sequence=UINT32_MAX;previous.minimum_free=UINT32_MAX;
 for(unsigned i=0;i<TRACE_SLOTS;i++){
  uint32_t seq=UINT32_MAX-i;trace_record_t *r=&previous.records[(seq-1)%TRACE_SLOTS];
  *r=(trace_record_t){seq,HTTP_TRACE_REPLY_RETURN,UINT32_MAX,INT32_MIN,INT32_MIN,UINT32_MAX,~seq};
 }
 assert(decode(body,sizeof(body),&data));assert(phv_get(&data,"records",&records)&&records.count==TRACE_SLOTS);
 assert(sizeof(retained)<=512);
 puts("TC1 retained trace: 10000 events, warm/cold/build change, torn commit, errno preserved, max formatting and memory budget: OK");
}
'''.replace('HOOKS',hooks).replace('SOURCE',source))

if __name__=='__main__':unittest.main()

"""Product-specialized diagnostics must match the unpruned cc9ca4af wire bytes."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/device_status.h"
void reference_device_diagnostics_mp(ph_writer *,const device_status_t *,const device_status_full_t *);
static int budget=-1,blocks;
void *ph_wire_malloc(size_t n){if(!budget)return NULL;if(budget>0)--budget;void *p=malloc(n);if(p)blocks++;return p;}
void ph_wire_free(void *p){if(p){assert(blocks>0);blocks--;free(p);}}
typedef void (*encoder)(ph_writer *,const device_status_t *,const device_status_full_t *);
static ph_buf *owned(encoder emit,const device_status_t *s,const device_status_full_t *f,int fail){
 ph_writer w;budget=fail;if(phw_init(&w,PH_WIRE_MSGPACK))emit(&w,s,f);else phw_discard(&w);
 ph_buf *b=phw_take(&w);budget=-1;return b;
}
static void check(device_status_t *s,device_status_full_t *f){
 ph_buf *a=owned(reference_device_diagnostics_mp,s,f,-1),*b=owned(device_diagnostics_mp,s,f,-1);
 assert(a&&b&&a->len==b->len&&!memcmp(a->buf,b->buf,a->len));
 ph_writer x,y;ph_buf xm,ym;assert(phw_init_measure(&x,1,&xm)&&phw_init_measure(&y,1,&ym));
 budget=0;reference_device_diagnostics_mp(&x,s,f);device_diagnostics_mp(&y,s,f);budget=-1;
 assert(phw_finish_fixed(&x)==a->len&&phw_finish_fixed(&y)==a->len);
 size_t sizes[]={1,5,a->len,a->len+1,4096};
 for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++){
  unsigned char left[4098],right[4098];memset(left,0xa5,sizeof(left));memset(right,0xa5,sizeof(right));
  assert(phw_init_fixed(&x,1,&xm,left+1,sizes[i])&&phw_init_fixed(&y,1,&ym,right+1,sizes[i]));
  budget=0;reference_device_diagnostics_mp(&x,s,f);device_diagnostics_mp(&y,s,f);budget=-1;
  assert(x.failed==y.failed&&xm.len==ym.len&&phw_finish_fixed(&x)==phw_finish_fixed(&y));
  assert(!memcmp(left,right,sizeof(left))&&left[0]==0xa5&&left[sizes[i]+1]==0xa5);
 }
 ph_buf_free(a);ph_buf_free(b);assert(!blocks);
 for(int fail=0;fail<12;fail++){
  a=owned(reference_device_diagnostics_mp,s,f,fail);b=owned(device_diagnostics_mp,s,f,fail);
  assert((a!=NULL)==(b!=NULL));if(a){assert(a->len==b->len&&!memcmp(a->buf,b->buf,a->len));ph_buf_free(a);ph_buf_free(b);}assert(!blocks);
 }
 /* Failure stays sticky; invalid capture/null arguments cannot publish output. */
 assert(phw_init_fixed(&x,1,&xm,(char[64]){0},64));x.failed=true;device_diagnostics_mp(&x,s,f);assert(x.failed&&!phw_finish_fixed(&x));
 assert(!owned(device_diagnostics_mp,NULL,f,-1));assert(!owned(device_diagnostics_mp,s,NULL,-1));
 uint32_t captured=s->captured;s->captured=0;assert(!owned(device_diagnostics_mp,s,f,-1));s->captured=captured;
 device_diagnostics_mp(NULL,s,f);assert(!blocks);
}
int main(void){
 device_status_t s={0};device_status_full_t f={0};s.model=TEST_MODEL;s.captured=STATUS_DIAGNOSTIC;
 for(unsigned n=0;n<32;n++){
  s.now_ms=n?UINT_MAX/(n+1):UINT_MAX;
  f.runtime.reset_valid=(n&1)!=0;f.runtime.reset_reason=-(int)n;
  f.runtime.memory_valid=(n&2)!=0;f.runtime.free_memory=INT_MAX-(int)n;f.runtime.allocated_memory=-(int)n;
  f.runtime.free_memory_chunks=(int)n;f.runtime.minimum_free=(int)n*137;
  f.watchdog.reset_name=n&1?"software/复位":"";f.watchdog.reset_raw=n;f.watchdog.enabled=(n&2)!=0;
  f.watchdog.ready=(n&4)!=0;f.watchdog.failed=(n&8)!=0;f.watchdog.fault=n&1?"fault\"\\\n":"";
  f.http.accepted=UINT_MAX-n;f.http.accept_failures=n*101;f.http.accept_errno=-(int)n;f.http.last_fd=-1;
  f.http.queued=n;f.http.rejected=n+7;f.http.nonblock_failures=n;f.http.drained=n;f.http.busy_sent=n;
  f.http.busy_failed=n;f.http.busy_errno=-(int)n;f.http.busy_bytes=n*23;
  f.http.set_rc=-(int)n;f.http.set_errno=(int)n;f.http.limit_get_rc=n&1?-1:0;f.http.limit=4;
  f.http.connections_get_rc=n&2?-1:0;f.http.connections=(int)n;
  f.network.mqtt_errors=n*431;strcpy(f.network.mqtt_last_error,"error/错误\"\\\n");
  f.ota.signature_verified=(n&1)!=0;f.ota.image_verified=(n&2)!=0;f.ota.key_id=UINT_MAX-n;
  f.ota.image_length=UINT_MAX-n;strcpy(f.ota.release_counter,"18446744073709551615");strcpy(f.ota.version,"rc.4/版本");
#if TEST_MODEL == 0
  f.model.a1.panel_errors=UINT_MAX-n;f.model.a1.hardware_errors=n;f.model.a1.panel_last_error=-(int)n;
  for(unsigned i=0;i<3;i++)f.model.a1.panel_raw[i]=(unsigned char)(n*13+i*79);
#elif TEST_MODEL == 1
  f.model.m1.rx_frames=UINT_MAX-n;f.model.m1.unknown_frames=n;f.model.m1.uart_errors=n;f.model.m1.discarded_bytes=n;
  for(unsigned i=0;i<20;i++)f.model.m1.last_frame[i]=(unsigned char)(n*13+i*79);
  s.sensor.m1.sample.pm25_status=n;s.sensor.m1.sample.climate_status=n;s.sensor.m1.sample.auxiliary_status=n;
  strcpy(s.sensor.m1.co2.serial,"0a12bc34de56");s.sensor.m1.co2.errors=UINT_MAX-n;s.sensor.m1.co2.last_error=-(int)n;
  f.model.m1.co2_samples=n;f.model.m1.co2_crc_errors=n;f.model.m1.co2_bus_initialized=(n&4)!=0;
  f.model.m1.co2_last_command=(uint16_t)(n*631);f.model.m1.co2_error_phase=n;
  s.sensor.m1.barometer.chip_id=n&1?0x50:0;s.sensor.m1.barometer.address=0x76;
  s.sensor.m1.barometer.samples=n;s.sensor.m1.barometer.errors=n;s.sensor.m1.barometer.attempts=n;s.sensor.m1.barometer.last_error=-(int)n;
#endif
#ifdef PHICOMM_BEMFA
  s.network.bemfa_errors=UINT_MAX-n;s.network.bemfa_last_stage=n;s.network.bemfa_last_error=-(int)n;
#endif
  f.service_memory_valid=(n&16)!=0;
  f.service_memory.reserved=UINT_MAX-n;f.service_memory.active=n;f.service_memory.denied=n;
  f.service_memory.waiting=n;f.service_memory.minimum_free=n;f.service_memory.mqtt_pending=n;
  f.service_memory.mqtt_peak=n;f.service_memory.mqtt_overflow=n;f.service_memory.mqtt_max_delay=n;
  f.service_memory.wire_alloc_failed=n;f.service_memory.wire_guard_denied=n;
  check(&s,&f);
 }
 return 0;
}
'''


class DiagnosticsModelPruning(unittest.TestCase):
    def test_actual_writer_bytes_and_failure_parity(self):
        for model in range(3):
            for fresh, resources, bemfa in ((0,1,0),(0,1,1),(0,0,1),(1,1,1),(1,0,1)):
                with self.subTest(model=model, fresh=fresh, resources=resources, bemfa=bemfa), tempfile.TemporaryDirectory(prefix='diag-model-') as td:
                    path=Path(td);source=path/'test.c';source.write_text(HARNESS)
                    flags=['-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-unused-function','-DPHICOMM_APPLICATION_WATCHDOG=1','-DPHICOMM_STATUS_MODEL='+str(model),'-DTEST_MODEL='+str(model),'-DPHICOMM_FULL_RESOURCES='+str(resources),'-I'+str(ROOT),'-I'+str(ROOT/'common')]
                    if fresh:flags+=['-DA1_FRESH_AIR=1']
                    if bemfa:flags+=['-DPHICOMM_BEMFA=1']
                    if os.environ.get('SANITIZE','0')=='1':flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
                    command=shlex.split(os.environ.get('CC','cc'))+flags+[str(source),*[str(ROOT/p) for p in ('common/device_status.c','common/typed_wire.c','common/wire_buffer.c','tests/diagnostics/reference_diagnostics.c')],'-lm','-o',str(path/'test')]
                    built=subprocess.run(command,text=True,capture_output=True,timeout=60)
                    self.assertEqual(built.returncode,0,built.stdout+built.stderr)
                    ran=subprocess.run([str(path/'test')],text=True,capture_output=True,timeout=60)
                    self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)


if __name__=='__main__':unittest.main()

"""Pinned ARM32/nano native status overlap, with an explicit allocation ledger.

The ledger is align8(request)+8 and counts copy-before-free output growth. It is
not a closed MiCO heap or a runtime/MQTT scheduler simulator.
"""
import os,shutil,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
HARNESS=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "common/device_status.h"
static size_t live,peak,capacity=100000,denied;
void *ph_wire_malloc(size_t n){size_t cost=((n+7)&~(size_t)7)+8;if(cost>capacity-live){denied++;return NULL;}size_t*p=malloc(cost);if(!p)return NULL;*p=cost;live+=cost;if(live>peak)peak=live;return (unsigned char*)p+8;}
void ph_wire_free(void*p){if(p){size_t*h=(size_t*)((unsigned char*)p-8);assert(*h<=live);live-=*h;free(h);}}
static ph_buf*status(unsigned model){
 device_status_t s={0};device_status_full_t f={0};s.model=model;s.captured=63;s.identity.version="1.0.0-rc.4-abcdef";strcpy(s.identity.name,"native status overlap");strcpy(s.identity.id,"aabbccddeeff");
 f.watchdog.fault="";f.watchdog.reset_name="unknown";
 ph_writer w;if(!phw_init(&w,1))return NULL;device_status_mp(&w,&s,&f);return phw_take(&w);
}
int main(void){
 assert(sizeof(void*)==4&&sizeof(size_t)==4&&sizeof(long)==4);
 for(unsigned model=0;model<3;model++){
  capacity=100000;peak=0;ph_buf*one=status(model);assert(one);ph_value v;assert(phv_parse(1,one->buf,one->len,&v));size_t retained=live;assert(retained&&one->len<16384);
  ph_buf*two=status(model);assert(two);size_t overlapped=peak;ph_buf_free(two);assert(live==retained);
  /* While the publish-side response owns its bytes, every allocator refusal
   * in a second HTTP-side encode must leave that first response intact. */
  for(size_t budget=retained;budget<=overlapped+128;budget+=128){capacity=budget;two=status(model);if(two){assert(phv_parse(1,two->buf,two->len,&v));ph_buf_free(two);}assert(live==retained);assert(phv_parse(1,one->buf,one->len,&v));}
  capacity=100000;two=status(model);assert(two);ph_buf_free(two);ph_buf_free(one);assert(!live&&denied);
  char out[160];int n=snprintf(out,sizeof(out),"ARM32 model=%u retained=%u overlap_peak=%u denied=%u cleanup=0 PASS\n",model,(unsigned)retained,(unsigned)overlapped,(unsigned)denied);extern int _write(int,const void*,int);_write(1,out,n);
 }
 return 0;
}
'''
class NativeStatusARM(unittest.TestCase):
 def test_pinned_arm32_status_overlap_oom_recovery(self):
  cc=Path(os.environ.get('ARM_CC',ROOT/'build/a1-tools/gcc-arm-none-eabi-5_4-2016q2/bin/arm-none-eabi-gcc'))
  qemu=shutil.which('qemu-arm') or shutil.which('qemu-arm-static')
  self.assertTrue(cc.is_file(),'prepare pinned A1 compiler or set ARM_CC')
  self.assertTrue(qemu,'qemu-arm is required for ARM32 regression')
  with tempfile.TemporaryDirectory() as tmp:
   d=Path(tmp);src=d/'test.c';src.write_text(HARNESS);exe=d/'test.elf'
   cmd=[str(cc),'-std=gnu99','-Os','-mthumb','-mcpu=cortex-m3','-DPHICOMM_APPLICATION_WATCHDOG=1','-I'+str(ROOT),'-nostartfiles','--specs=nano.specs','--specs=nosys.specs','-Wl,-e,_start,-Ttext=0x10000',str(src),*[str(ROOT/p) for p in ('common/device_status.c','common/typed_wire.c','common/wire_buffer.c','tests/http_core/json_arm_runtime.c')],'-lm','-o',str(exe)]
   result=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
   result=subprocess.run([qemu,str(exe)],capture_output=True,text=True,timeout=30);self.assertEqual(result.returncode,0,result.stdout+result.stderr);print(result.stdout)
if __name__=='__main__':unittest.main()

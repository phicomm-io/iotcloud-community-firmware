"""Actual A1/M1 metadata writers: no status capture, SDK or JSON graph."""
from pathlib import Path
import os, shlex, subprocess, tempfile, unittest
from tests.test_http_json_budget import function
ROOT=Path(__file__).resolve().parents[1]
class ProductMetadata(unittest.TestCase):
 def test_actual_info_and_capabilities_variants_restriction_and_oom(self):
  product=(ROOT/'common/service_product.inc').read_text()
  arrays=product[product.index('static const char *const svc_capability_actions[]'):product.index('#include "service_product_mp.inc"')]
  source=(ROOT/'common/service_product_mp.inc').read_text()
  writers=function(source,'void service_product_info_mp(')+'\n'+function(source,'void service_product_capabilities_mp(')
  pre=r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "common/service_mp.h"
#include "common/firmware_variant_defs.h"
static bool recovery;
bool device_config_recovery(void){return recovery;}
#define DEVICE_ID "aabbccddeeff"
#define DEVICE_VERSION "1.0.0-rc.4-abcdef"
static long budget=-1,live;
void *ph_wire_malloc(size_t n){if(!budget)return NULL;if(budget>0)budget--;void *p=malloc(n);if(p)live++;return p;}
void ph_wire_free(void *p){if(p){live--;free(p);}}
'''
  body=r'''
static void text(ph_value *o,const char *key,const char *expected){ph_value v;assert(phv_get(o,key,&v)&&phv_equal_text(&v,expected));}
static bool action(ph_value *o,const char *name){ph_value a,v;assert(phv_get(o,"actions",&a));for(unsigned i=0;i<a.count;i++){assert(phv_at(&a,i,&v));if(phv_equal_text(&v,name))return true;}return false;}
int main(void){
 const char *extra[]={"ota.check","ota.install",NULL};
 for(unsigned restricted=0;restricted<2;restricted++)for(unsigned recover=0;recover<2;recover++){
  recovery=recover;ph_writer w;assert(phw_init(&w,1));service_product_info_mp(&w);ph_buf *b=phw_take(&w);assert(b);ph_value info,v;
  assert(phv_parse(1,b->buf,b->len,&info));text(&info,"model",DEVICE_MODEL);text(&info,"device_id",DEVICE_ID);text(&info,"version",DEVICE_VERSION);text(&info,"firmware_version",DEVICE_VERSION);text(&info,"variant",PHICOMM_FIRMWARE_VARIANT);text(&info,"protocol","tmcp");
  int64_t n;assert(phv_get(&info,"protocol_version",&v)&&phv_int(&v,&n)&&n==2);text(&info,"default_wire_format","msgpack");assert(phv_get(&info,"wire_formats",&v)&&v.count==1);ph_buf_free(b);
  assert(phw_init(&w,1));service_product_capabilities_mp(&w,extra,restricted);b=phw_take(&w);assert(b);ph_value caps;assert(phv_parse(1,b->buf,b->len,&caps));
  text(&caps,"model",DEVICE_MODEL);text(&caps,"device_id",DEVICE_ID);text(&caps,"firmware_version",DEVICE_VERSION);text(&caps,"variant",PHICOMM_FIRMWARE_VARIANT);text(&caps,"firmware_variant",PHICOMM_FIRMWARE_VARIANT);
  assert(action(&caps,"ota.check")&&action(&caps,"ota.install"));assert(action(&caps,"config.set")==!restricted);
  bool yes;assert(phv_get(&caps,"full_resources",&v)&&phv_bool(&v,&yes)&&yes==PHICOMM_FULL_RESOURCES);assert(phv_get(&caps,"bootstrap_ui",&v)&&phv_bool(&v,&yes)&&yes==!PHICOMM_FULL_RESOURCES);
  if(!restricted){assert(phv_get(&caps,"config_recovery",&v)&&phv_bool(&v,&yes)&&yes==recovery);
#ifdef FEATURE_DEVICE_A1
   assert(action(&caps,"fan.set"));assert(phv_get(&caps,"button_keys",&v)&&v.count==3);
#ifdef A1_FRESH_AIR
   assert(action(&caps,"fresh_air.set"));
#else
   assert(!action(&caps,"fresh_air.set"));
#endif
#else
   assert(action(&caps,"wake_alarm.stop"));assert(!action(&caps,"fan.set"));
#endif
  }else assert(!phv_get(&caps,"config_recovery",&v));
  ph_buf_free(b);assert(!live);
 }
 for(int cut=0;cut<20;cut++){
  budget=cut;ph_writer w;if(phw_init_capacity(&w,1,1)){service_product_capabilities_mp(&w,extra,false);ph_buf *b=phw_take(&w);if(b)ph_buf_free(b);}else phw_discard(&w);budget=-1;assert(!live);
 }return 0;
}
'''
  with tempfile.TemporaryDirectory() as temp:
   t=Path(temp)
   variants=[(model,full,fresh,bemfa) for model in ('a1','m1') for full in (0,1) for fresh in ((0,1) if model=='a1' else (0,)) for bemfa in ((0,1) if model=='a1' else (0,))]
   for model,full,fresh,bemfa in variants:
    flags=['-DDEVICE_MODEL="'+model+'"','-DPHICOMM_FULL_RESOURCES='+str(full)]
    if model=='a1':flags+=['-DFEATURE_DEVICE_A1=1']
    if bemfa:flags+=['-DPHICOMM_BEMFA=1']
    if fresh:flags+=['-DA1_FRESH_AIR=1']
    (t/'test.c').write_text(pre+arrays+writers+body)
    cmd=shlex.split(os.environ.get('CC','cc'))+['-std=gnu99','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(ROOT),'-I'+str(ROOT/'common'),*flags,str(t/'test.c'),str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c'),'-lm','-o',str(t/'test')]
    with self.subTest(model=model,full=full,fresh=fresh,bemfa=bemfa):
     built=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(built.returncode,0,built.stderr)
     ran=subprocess.run([str(t/'test')],capture_output=True,text=True);self.assertEqual(ran.returncode,0,ran.stderr)

"""TC1 native status hook and the shared full-only dispatch contract.

Former full/live JSON entry was removed. Actual fixed SDK capture is exercised
by test_typed_tc1_status; reservation by test_tc1_status_guard. These probes own
hook failure propagation, native status validation, busy recovery and owned
cached replies rather than asserting an obsolete JSON entry exists.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tests.test_runtime_mqtt_mp import function
from tests import test_service_mp
ROOT=Path(__file__).resolve().parents[1]
class TC1StatusDispatch(unittest.TestCase):
 def test_actual_status_entry(self):
  hook=function((ROOT/'TC1/service.c').read_text(),'void service_product_status_mp(')
  source=r'''
#include <assert.h>
#include <string.h>
#include "common/device_status.h"
static bool busy;
static unsigned captures;
bool tc1_status_capture(device_status_t*s,device_status_full_t*f){
 captures++;assert(s&&f);if(busy)return false;memset(s,0,sizeof(*s));memset(f,0,sizeof(*f));
 s->model=DEVICE_TC1;s->captured=STATUS_IDENTITY|STATUS_NETWORK|STATUS_CONTROL|STATUS_OTA|STATUS_DIAGNOSTIC;
 s->identity.version="rc4";strcpy(s->identity.id,"aabbccddeeff");strcpy(s->identity.name,"TC1");return true;
}
HOOK
int main(void){
 unsigned char bytes[4096];ph_writer w;ph_buf storage;busy=true;
 assert(phw_init_fixed(&w,1,&storage,bytes,sizeof(bytes)));service_product_status_mp(&w);assert(w.failed&&!phw_finish_fixed(&w));
 busy=false;assert(phw_init_fixed(&w,1,&storage,bytes,sizeof(bytes)));service_product_status_mp(&w);size_t n=phw_finish_fixed(&w);assert(n&&captures==2);
 ph_value root,v;assert(phv_parse(1,bytes,n,&root));assert(phv_get(&root,"model",&v)&&phv_equal_text(&v,"tc1"));assert(phv_get(&root,"socket_states",&v)&&v.count==6);
 return 0;
}
'''.replace('HOOK',hook)
  with tempfile.TemporaryDirectory() as tmp:
   p=Path(tmp);(p/'test.c').write_text(source);exe=p/'test'
   subprocess.run(['cc','-std=gnu99','-g','-fsanitize=address,undefined','-no-pie','-I'+str(ROOT),str(p/'test.c'),*[str(ROOT/f) for f in ('common/device_status.c','common/typed_wire.c','common/wire_buffer.c')],'-lm','-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True)
 def test_native_validation_and_busy_recovery(self):
  case=test_service_mp.NativeService()
  case.test_malformed_requests_ids_and_status_parameters()
  case.test_busy_capture_recovery_and_independent_cached_responses()
 def test_actual_info_and_capabilities_identity_recovery_and_bounds(self):
  source=(ROOT/'TC1/service.c').read_text()
  hooks='\n'.join(function(source,s) for s in ('void service_product_info_mp(', 'void service_product_capabilities_mp('))
  fixture=r'''
#include <assert.h>
#include <string.h>
#include "common/typed_wire.h"
#include "common/diagnostics_ids.h"
#define VERSION "rc4-metadata-host"
#define maxNameLen 32
#define PHICOMM_FIRMWARE_VARIANT "full"
static char str_mac[]="aabbccddeeff";
static bool recovery;
static bool device_config_recovery(void){return recovery;}
HOOKS
static void text(const ph_value*p,const char*k,const char*s){ph_value v;assert(phv_get(p,k,&v)&&phv_equal_text(&v,s));}
int main(void){
 unsigned char bytes[8192];ph_buf storage;ph_writer w;ph_value root,v,item;bool yes;int64_t value;
 assert(phw_init_fixed(&w,1,&storage,bytes,sizeof(bytes)));service_product_info_mp(&w);size_t n=phw_finish_fixed(&w);assert(n&&phv_parse(1,bytes,n,&root));
 text(&root,"model","tc1");text(&root,"device_id",str_mac);text(&root,"version",VERSION);
 for(unsigned restricted=0;restricted<2;restricted++)for(unsigned r=0;r<2;r++){
  recovery=r;const char*extra[]={"lan.pair",NULL};assert(phw_init_fixed(&w,1,&storage,bytes,sizeof(bytes)));service_product_capabilities_mp(&w,extra,restricted);n=phw_finish_fixed(&w);assert(n&&phv_parse(1,bytes,n,&root));
  text(&root,"model","tc1");text(&root,"device_id",str_mac);text(&root,"firmware_version",VERSION);text(&root,"variant","full");text(&root,"mqtt_protocol","tmcp");
  assert(phv_get(&root,"mqtt_protocol_version",&v)&&phv_int(&v,&value)&&value==2);assert(phv_get(&root,"protocol",&v)&&phv_int(&v,&value)&&value==2);
  assert(phv_get(&root,"config_recovery",&v)&&phv_bool(&v,&yes)&&yes==(bool)r);assert(phv_get(&root,"dual_mqtt",&v)&&phv_bool(&v,&yes)&&yes);
  assert(phv_get(&root,"config_backup",&v)&&phv_int(&v,&value)&&value==1);assert(phv_get(&root,"status_views",&v)&&v.count==1&&phv_at(&v,0,&item)&&phv_equal_text(&item,"full"));
  assert(phv_get(&root,"diagnostics_schema",&v)&&phv_int(&v,&value)&&value==PH_DIAGNOSTICS_SCHEMA);
  assert(phv_get(&root,"actions",&v));assert(v.count==(restricted?4:25));assert(phv_at(&v,v.count-1,&item)&&phv_equal_text(&item,"lan.pair"));
  if(restricted){const char*names[]={"info.get","status.get","capabilities.get"};for(unsigned i=0;i<3;i++)assert(phv_at(&v,i,&item)&&phv_equal_text(&item,names[i]));}
  /* Fixed output OOM is sticky; no truncated object can become publishable. */
  assert(!phw_init_fixed(&w,1,&storage,bytes,0));
  for(size_t capacity=1;capacity<n;capacity++){
   assert(phw_init_fixed(&w,1,&storage,bytes,capacity));service_product_capabilities_mp(&w,extra,restricted);assert(!phw_finish_fixed(&w));
  }
 }
 return 0;
}
'''.replace('HOOKS',hooks)
  with tempfile.TemporaryDirectory() as tmp:
   p=Path(tmp);(p/'probe.c').write_text(fixture);exe=p/'probe'
   subprocess.run(['cc','-std=gnu99','-g','-fsanitize=address,undefined','-no-pie','-I'+str(ROOT),str(p/'probe.c'),str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c'),'-lm','-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True)
if __name__=='__main__':unittest.main()

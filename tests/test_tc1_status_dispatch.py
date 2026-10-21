"""Actual lightweight TC1 service dispatch: validation, busy and delegation."""
import os
from pathlib import Path
import subprocess,tempfile,unittest
from tests.sdk_json import copy_patched_json
from tests.test_http_json_budget import function
ROOT=Path(__file__).resolve().parents[1]
class TC1StatusDispatch(unittest.TestCase):
 def test_actual_status_entry(self):
  source=function((ROOT/'TC1/service.c').read_text(),'json_object *service_product(const char *action')
  fixture=r'''
#include <assert.h>
#include <string.h>
#include "common/status_view.h"
static bool busy;static unsigned full,live,other;
static json_object *tc1_status_json(void){full++;return busy?NULL:json_object_new_object();}
static json_object *tc1_status_live_json(void){live++;return busy?NULL:json_object_new_object();}
static json_object *service_product_inner(const char*action,json_object*p,const char**code){assert(!strcmp(action,"unknown")&&p);other++;*code="unsupported";return NULL;}
PRODUCTION
int main(void){
 const char*inputs[]={"{}","{\"view\":\"full\"}","{\"view\":\"live\"}","{\"view\":1}","{\"other\":true}"};
 for(unsigned failure=0;failure<2;failure++){busy=failure;for(unsigned i=0;i<5;i++){
  json_object*p=json_tokener_parse(inputs[i]);const char*code=NULL;json_object*out=service_product("status.get",p,&code);
  assert(!strcmp(code,i>=3?"invalid_params":busy?"busy":"ok"));assert((out!=NULL)==(i<3&&!busy));if(out)json_object_put(out);json_object_put(p);
 }}
 assert(full==4&&live==2&&!other);json_object*p=json_object_new_object();const char*code;assert(!service_product("unknown",p,&code)&&!strcmp(code,"unsupported")&&other==1);json_object_put(p);return 0;
}
'''.replace('PRODUCTION',source)
  with tempfile.TemporaryDirectory() as tmp:
   d=Path(tmp);sdk=Path(os.environ.get('PHICOMM_TEST_JSON_DIR',ROOT/'build/a1-sdk/mico-os/libraries/utilities/json_c'));json=copy_patched_json(sdk,d/'sdk')
   for p in json.glob('*.c'):p.write_text(p.read_text().replace('%ld','%d'))
   (d/'probe.c').write_text(fixture)
   subprocess.run([os.environ.get('CC','cc'),'-std=gnu99','-g','-include','stdint.h','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(ROOT),'-I'+str(ROOT/'tests/stubs'),'-I'+str(json),str(d/'probe.c'),*[str(p) for p in json.glob('*.c')],'-o',str(d/'probe')],check=True)
   subprocess.run([str(d/'probe')],check=True,timeout=10)
if __name__=='__main__':unittest.main()

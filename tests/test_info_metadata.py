"""Actual metadata dispatch/HTTP envelopes on all products; no sockets or DUTs.

The old status-derived envelope is the compatibility oracle. Product status
and mutation work are observable boundaries, not alternative metadata builders.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tests.sdk_json import copy_patched_json
from tests.test_http_json_budget import function

ROOT = Path(__file__).resolve().parents[1]


def dispatch(model):
    source = (ROOT/('TC1/service.c' if model == 'TC1' else 'common/service_product.inc')).read_text()
    entry = function(source, 'json_object *service_product(const char *action')
    if model == 'TC1':
        return ('#define VERSION FIRMWARE_VERSION\nchar str_mac[16]="AAbBcCdDeEfFxxx";\n' +
                function(source, 'static void text(') + '\n' + entry)
    # Compile the actual model/ID/version aliases as well as the actual entry.
    section = source[source.index('#define DEVICE_MODEL "'+model.lower()+'"'):]
    section = section[:section.index('#else' if model == 'A1' else '#endif')]
    aliases = '\n'.join(line for line in section.splitlines() if line.startswith(
        ('#define DEVICE_MODEL ', '#define DEVICE_ID ', '#define DEVICE_VERSION ', '#define STATUS ', '#define STATUS_LIVE ')))
    header = (ROOT/model/(model.lower()+'.h')).read_text()
    version = re.search(r'^#define '+model+'_VERSION .+$', header, re.M).group()
    return (version+'\nchar '+model.lower()+'_id[13]="AAbBcCdDeEfF";\n'+aliases+'\n'+
            function(source, 'static void svc_string(')+'\n'+entry)


FIXTURE = r'''
#include "mico.h"
#include "common/service.c"
#include "common/httpd/product.h"
#include "tests/stubs/status_wire.inc"
#include "common/generated/build_version.h"
uint32_t test_now;
static unsigned attempts,fault_at,status_calls,inner_calls;
static bool allow_status;
int info_test_permit(size_t bytes){return json_heap_permit(bytes)&&(!fault_at||++attempts!=fault_at);}
static json_object *status(bool live){
 assert(allow_status);status_calls++;json_object *o=json_object_new_object();
 json_build_object_add(o,"model",json_object_new_string(MODEL_NAME));
 json_build_object_add(o,"version",json_object_new_string(FIRMWARE_VERSION));
 json_build_object_add(o,"device_id",json_object_new_string("AAbBcCdDeEfF"));
 json_build_object_add(o,"unrelated",json_object_new_string(live?"live":"full"));return o;
}
static json_object *a1_status_json(void){return status(false);}
static json_object *a1_status_live_json(void){return status(true);}
static json_object *m1_status_json(void){return status(false);}
static json_object *m1_status_live_json(void){return status(true);}
static json_object *tc1_status_json(void){return status(false);}
static json_object *tc1_status_live_json(void){return status(true);}
static json_object *service_product_inner(const char*a,json_object*p,const char**c){(void)p;assert(!strcmp(a,"unknown"));inner_calls++;*c="unsupported";return NULL;}
PRODUCTION_DISPATCH
json_object *service_product_capabilities(void){return json_object_new_object();}
json_object *automation_command(const char*a,json_object*p,const char**c,bool*h){(void)a;(void)p;(void)c;*h=false;return NULL;}
/* Frozen pre-change info envelope, with its status graph dependency retained. */
static json_object *old_info(json_object *p,const char *id){
 const char *code="ok";json_object *s=service_product("status.get",p,&code);
 if(!s)return result(id,code,NULL);
 json_object *o=json_object_new_object();const char *keys[]={"model","device_id","version"};
 for(unsigned i=0;i<3;i++){json_object *v=json_object_object_get(s,keys[i]);if(v)json_build_object_add(o,keys[i],json_object_get(v));}
 phicomm_variant_json(o);json_object_put(s);return result(id,"ok",o);
}
static void empty(void){json_heap_stats_t s;json_heap_snapshot(&s);assert(!s.live_blocks);}
static json_object *request(const char *params,const char *id){
 char input[300];snprintf(input,sizeof(input),"{\"action\":\"info.get\",\"params\":%s%s}",params,id?",\"request_id\":\"read-123\"":"");
 return service_parse(input,strlen(input));
}
static void parity(void){
 const char *inputs[]={"{}","{\"view\":\"full\"}","{\"view\":\"live\"}"};
 for(unsigned i=0;i<3;i++)for(unsigned identified=0;identified<2;identified++){
  const char *id=identified?"read-123":NULL;allow_status=true;
  json_object *p=json_tokener_parse(inputs[i]);struct printbuf *old=json_owned_to_wire(old_info(p,id));assert(old);json_object_put(p);
  char expected[768];assert(old->bpos<sizeof(expected));strcpy(expected,old->buf);printbuf_free(old);
  unsigned calls=status_calls;allow_status=false;json_object *q=request(inputs[i],id);assert(q);
  struct printbuf *wire=service_command_wire(q);assert(wire&&!strcmp(expected,wire->buf));json_object_put(q);printbuf_free(wire);
  assert(status_calls==calls&&!inner_calls);empty();
 }
 const char *invalid[]={"{\"view\":null}","{\"view\":1}","{\"view\":\"other\"}","{\"other\":true}","{\"view\":\"live\\u0000full\"}","null"};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++){
  json_object *q=request(invalid[i],NULL),*r=service_command(q);assert(r);
  assert(!strcmp(json_object_get_string(json_object_object_get(r,"code")),"invalid_params"));
  json_object_put(q);json_object_put(r);empty();
 }
 const char *code;json_object *p=json_object_new_object();assert(!service_product("unknown",p,&code)&&!strcmp(code,"unsupported")&&inner_calls==1);json_object_put(p);empty();
 printf("%s variant=%s: actual metadata dispatch, legacy full/live envelope bytes/order, bounded ID/case and validation PASS\n",MODEL_NAME,PHICOMM_FIRMWARE_VARIANT);
}
static void fault_sweep(void){
 unsigned failures=0;allow_status=false;
 for(unsigned nth=1;nth<300;nth++){
  attempts=0;fault_at=nth;ph_http_product p={0};ph_http_exchange x={0};p.failures=service_memory_failures();
  ph_http_query(&p,&x,"info.get");unsigned calls=attempts;fault_at=0;
  if(calls<nth){assert(x.response.status==200&&p.wire);ph_http_product_release(&p);empty();break;}
  assert(x.response.status==503&&!p.wire);ph_http_product_release(&p);failures++;empty();
  ph_http_product_init(&p,&x);p.failures=service_memory_failures();ph_http_query(&p,&x,"info.get");assert(x.response.status==200&&p.wire);ph_http_product_release(&p);empty();
 }
 assert(failures>20&&failures<299);
 printf("%s actual HTTP query: %u allocation refusals ->503, clean recovery, no partial success/leak PASS\n",MODEL_NAME,failures);
}
int main(void){service_init();assert(ready);parity();fault_sweep();return 0;}
'''


class InfoMetadata(unittest.TestCase):
    def test_actual_host_product_shims_keep_info_compatible(self):
        sdk = Path(os.environ.get('PHICOMM_TEST_JSON_DIR', ROOT/'build/a1-sdk/mico-os/libraries/utilities/json_c'))
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            json = copy_patched_json(sdk,directory/'sdk')
            for source in json.glob('*.c'):
                source.write_text(source.read_text().replace('%ld','%d'))
            sha = ROOT/'build/a1-sdk/mico-os/MiCO/security/SHAUtils'
            fixture = r'''
#include "mico.h"
#include "common/service.c"
uint32_t test_now;
void m1_set_brightness(unsigned n){(void)n;abort();}
void a1_control_local(const char*a,json_object*p){(void)a;(void)p;abort();}
void UserRelaySet(int i,bool on){(void)i;(void)on;abort();}
#include "tests/host/service_adapter.inc"
''' + function(FIXTURE,'static json_object *old_info(') + function(FIXTURE,'static void empty(') + r'''
int main(void){
 service_init();json_object *params=json_object_new_object();
 struct printbuf *old=json_owned_to_wire(old_info(params,NULL));assert(old);json_object_put(params);
 char expected[768];assert(old->bpos<sizeof(expected));strcpy(expected,old->buf);printbuf_free(old);empty();
 struct printbuf *wire=service_query_wire("info.get");assert(wire&&!strcmp(expected,wire->buf));
 assert(!strstr(wire->buf,"device_id"));printbuf_free(wire);empty();
 puts("Actual HOST product shim info legacy metadata/variant/envelope bytes PASS");return 0;
}
'''
            (directory/'host.c').write_text(fixture)
            for model in ('A1','M1','TC1'):
                with self.subTest(model=model):
                    binary=directory/model
                    subprocess.run([os.environ.get('CC','cc'),'-std=gnu99','-g',
                        '-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-DHOST_'+model,
                        '-I'+str(ROOT),'-I'+str(ROOT/'tests/stubs'),'-I'+str(sha),'-I'+str(json),
                        str(directory/'host.c'),str(ROOT/'common/json_owned.c'),str(sha/'sha224-256.c'),
                        *map(str,json.glob('*.c')),'-o',str(binary)],check=True,timeout=60)
                    subprocess.run([str(binary)],check=True,timeout=20)

    def test_actual_dispatch_envelope_and_failure_boundaries(self):
        sdk = Path(os.environ.get('PHICOMM_TEST_JSON_DIR', ROOT/'build/a1-sdk/mico-os/libraries/utilities/json_c'))
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            json = copy_patched_json(sdk,directory/'sdk')
            for source in json.glob('*.c'):
                source.write_text(source.read_text().replace('%ld','%d'))
            heap = json/'json_heap.c'
            heap.write_text('#include <stddef.h>\nint info_test_permit(size_t);\n'+heap.read_text().replace('if (!json_heap_permit(', 'if (!info_test_permit('))
            sha = ROOT/'build/a1-sdk/mico-os/MiCO/security/SHAUtils'
            for model in ('A1','M1','TC1'):
                variants = ((1,0),(0,0),(1,1),(0,1)) if model == 'A1' else ((1,0),(0,0))
                for full,fresh in variants:
                    with self.subTest(model=model,full=full,fresh=fresh):
                        source = directory/(model+str(full)+str(fresh)+'.c')
                        source.write_text(FIXTURE.replace('PRODUCTION_DISPATCH',dispatch(model)).replace('MODEL_NAME','"'+model.lower()+'"'))
                        binary=source.with_suffix('')
                        subprocess.run([os.environ.get('CC','cc'),'-std=gnu99','-g',
                            '-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',
                            '-DPHICOMM_FULL_RESOURCES='+str(full),*(['-DA1_FRESH_AIR=1'] if fresh else []),
                            '-I'+str(ROOT),'-I'+str(ROOT/'tests/stubs'),'-I'+str(sha),'-I'+str(json),
                            str(source),str(ROOT/'common/json_owned.c'),str(ROOT/'common/httpd/product.c'),
                            str(sha/'sha224-256.c'),*map(str,json.glob('*.c')),'-o',str(binary)],check=True,timeout=60)
                        subprocess.run([str(binary)],check=True,timeout=20)


if __name__ == '__main__':
    unittest.main()

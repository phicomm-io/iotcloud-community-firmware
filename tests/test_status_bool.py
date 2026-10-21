"""Request-local primitive sharing: actual SDK bytes, references and OOM cleanup."""
import unittest
from tests import test_http_json_budget as budget
from tests.test_http_json_budget import ROOT, function, status_fixture


def scalar_fixture():
    helper=function((ROOT/'common/device_status.c').read_text(),'static void status_bool(')
    return r'''
#include <stdint.h>
#include <stdbool.h>
#include "json.h"
#include "json_heap.h"
#include "common/json_owned.h"
#include "common/json_build.h"
#include "printbuf.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned allocation,fail_at;
int json_heap_permit(size_t n){(void)n;return !fail_at || ++allocation!=fail_at;}
''' + helper + r'''
static json_object *build(bool shared){
 json_object *o=json_object_new_object();
 for(unsigned i=0;i<40;i++){
  char key[20];snprintf(key,sizeof(key),"boolean%u",i);
  if(shared)status_bool(o,key,i%3==0);
  else json_build_object_add(o,key,json_object_new_boolean(i%3==0));
 }
 return o;
}
static void clean(void){json_heap_stats_t stats;json_heap_snapshot(&stats);assert(!stats.live_blocks);}
int main(void){
 char golden[2048];json_object *old=build(false);assert(old);
 const char *text=json_object_to_json_string(old);assert(text && strlen(text)<sizeof(golden));strcpy(golden,text);json_object_put(old);clean();
 json_object *o=build(true);assert(o);
 assert(json_object_object_get(o,"boolean0")==json_object_object_get(o,"boolean3"));
 assert(json_object_object_get(o,"boolean1")==json_object_object_get(o,"boolean2"));
 assert(json_object_object_get(o,"boolean0")!=json_object_object_get(o,"boolean1"));
 struct printbuf *wire=json_owned_to_wire(o);assert(wire&&!strcmp(golden,wire->buf));printbuf_free(wire);clean();
 for(unsigned point=1;point<=220;point++){
  allocation=0;fail_at=point;o=build(true);wire=json_owned_to_wire(o);fail_at=0;
  if(wire){assert(!strcmp(golden,wire->buf));printbuf_free(wire);}clean();
 }
 o=build(true);wire=json_owned_to_wire(o);assert(wire&&!strcmp(golden,wire->buf));printbuf_free(wire);clean();
 puts("actual status_bool primitive aliases: byte parity, 220 allocator faults and recovery PASS");return 0;
}
'''


def full_fixture():
    fixture=status_fixture();fixture=fixture[:fixture.index('int main(void){')]
    source=(ROOT/'common/device_status.c').read_text()
    helper=function(source,'static void status_bool(')
    replacement='static bool shared_booleans;\n'+helper.replace('static void status_bool(', 'static void status_bool_shared(',1)+r'''
static void status_bool(json_object *o,const char *key,bool value){
 if(shared_booleans)status_bool_shared(o,key,value);
 else json_build_object_add(o,key,json_object_new_boolean(value));
}
'''
    source=source.replace(helper,replacement)
    for header in ('device_status.h','json_build.h','firmware_variant.h'):
        source=source.replace('#include "'+header+'"','#include "common/'+header+'"')
    fixture=fixture.replace('#include "common/device_status.c"',source)
    return fixture+r'''
int main(void){
 char warm[64],golden[4096];snprintf(warm,sizeof(warm),"%f",1.25);libc_baseline=live;healthy_stats();
 for(unsigned variant=0;variant<3;variant++){
  setup(variant);capacity=libc_baseline+100000;shared_booleans=false;
  struct printbuf *old=json_owned_to_wire(service_query("status.get"));assert(old && strlen(old->buf)<sizeof(golden));strcpy(golden,old->buf);printbuf_free(old);healthy_stats();
  shared_booleans=true;struct printbuf *wire=json_owned_to_wire(service_query("status.get"));assert(wire && !strcmp(golden,wire->buf));printbuf_free(wire);healthy_stats();
 }
 puts("actual full A1 status ordinary/max/escaped: unchanged wire and zero retained JSON PASS");return 0;
}
'''


class StatusBoolean(unittest.TestCase):
    def test_sdk_scalar_aliases_faults_and_release(self):
        for model in ('A1','M1','TC1'):
            with self.subTest(model=model):
                budget.JsonBudget().compile_arm(model,scalar_fixture(),budget=False,product=False)

    def test_full_status_wire_parity(self):
        budget.JsonBudget().compile_arm('A1',full_fixture(),budget=True)

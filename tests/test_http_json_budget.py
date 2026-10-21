"""Actual A1 status graph + pinned SDK JSON/ARM32 nano memory regression.

The ledger models each native allocation as align8(request)+8 bytes and counts
copy-before-free realloc peaks. It is not a MiCO heap emulator or a guarantee
against concurrent closed SDK allocations/fragmentation. No guard is lowered.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from tests.sdk_json import copy_patched_json

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def status_fixture():
    source = (ROOT/'A1/http.c').read_text()
    builders = source[source.index('static void str('):source.index('static void memory_response(')]
    watchdog = function((ROOT/'common/watchdog.c').read_text(), 'void watchdog_status_json(')
    filter_fn = function((ROOT/'common/service_product.inc').read_text(), 'json_object *a1_filter_json(')
    snapshots = r'''
#include "common/watchdog.h"
static bool enabled=true,failed,startup_complete=true;
static const char *fault="";static int reset_raw=0x30;
static struct {bool seen;} points[WATCHDOG_SLOT_COUNT]={{true},{true},{true},{true}};
static uint32_t watchdog_lock(void){return 0;}
static void watchdog_unlock(uint32_t n){(void)n;}
static const char *reset_name(void){return "software";}
void a1_filter_get(a1_filter_storage_t *p){memset(p,0,sizeof(*p));p->lifetime_hours=2000;}
'''
    service = (ROOT/'common/service.c').read_text()
    result = function(service, 'static json_object *result(')
    query = function(service, 'json_object *service_query(')
    result = 'static void string(json_object*o,const char*k,const char*v){json_build_object_add(o,k,json_object_new_string(v));}\n' + result
    result += r'''
json_object *service_command(json_object *request){
 const char *id=NULL;json_object *rid=json_object_object_get(request,"request_id");
 if(rid)id=json_object_get_string(rid);
 return result(id,"ok",a1_status_json());
}
'''
    mqtt=(ROOT/'common/mqtt_service.inc').read_text()
    wire=function(mqtt,'static struct printbuf *service_mqtt_wire(').replace('service_mqtt_wire(', 'production_mqtt_wire(',1)
    mqtt_fixture=r'''
#define MQTT_SUCCESS 0
#define MQTT_FAILURE -1
static bool mqtt_modern;static unsigned mqtt_bytes,service_mqtt_reply_count;
static int service_mqtt_flush(int (*sender)(const char*,const char*,bool)){(void)sender;return MQTT_SUCCESS;}
''' + wire + r'''
static struct printbuf *service_mqtt_wire(json_object *o){
 if(mqtt_modern)return production_mqtt_wire(o);
 struct printbuf *wire=o?json_object_to_json_string_ex(o):NULL;json_object_put(o);return wire;
}
static int mqtt_sender(const char *topic,const char *wire,bool retain){
 assert(retain&&strstr(topic,"/state"));assert(strstr(wire,"\"model\": \"a1\""));mqtt_bytes=strlen(wire);return MQTT_SUCCESS;
}
''' + function(mqtt,'static int service_mqtt_publish(') + '\n' + function(mqtt,'static int service_mqtt_snapshot(')
    fixture = (ROOT/'tests/http_core/status_budget.c').read_text().replace('/* PRODUCTION_MQTT_WIRE */',mqtt_fixture)
    return fixture.replace('/* PRODUCTION_WATCHDOG_FILTER */', snapshots+watchdog+'\n'+filter_fn).replace('/* PRODUCTION_BUILDERS */', builders).replace('/* PRODUCTION_RESULT_QUERY */', result+'\n'+query)


def background_fixture():
    runtime=(ROOT/'common/runtime.c').read_text()
    engine=(ROOT/'common/engine.c').read_text()
    functions='\n'.join(function(engine,s) for s in (
        'bool feature_sample_fresh(', 'unsigned feature_aggregate(',
        'bool feature_co2_fresh(', 'unsigned feature_co2_aggregate('))
    functions+='\n#define number integer\n'+'\n'.join(function(runtime,s) for s in (
        'static void co2_body(', 'static json_object *sample_body(',
        'static json_object *average_body(', 'static void direct_json('))+'\n#undef number\n'
    fixture=status_fixture();fixture=fixture[:fixture.index('int main(void){')]
    return fixture+'\n#include <math.h>\n'+functions+r'''
static char background_golden[2048];
static bool nested_ok,owned_background;
static int nested_status(const char *topic,const char *body,bool retain){
 assert(!strcmp(topic,"test/average")&&!retain&&!strcmp(body,background_golden));
 json_heap_stats_t stats;json_heap_snapshot(&stats);
 assert(owned_background?stats.live_blocks==2:stats.live_blocks>2);
 json_object *reply=service_query("status.get");
 struct printbuf *wire=json_owned_to_wire(reply);
 nested_ok=wire!=NULL;if(wire)printbuf_free(wire);
 assert(!strcmp(body,background_golden));return -7;
}
static json_object *background_graph(void){
 feature_sample_t samples[3]={{.valid=true,.received_ms=100,.value={22,45,100,.05},
  .co2_supported=true,.co2_valid=true,.co2=1100,.co2_received_ms=100}};
 return average_body(samples,3,100);
}
int main(void){
 char warm[64];snprintf(warm,sizeof(warm),"%f",1.25);libc_baseline=live;healthy_stats();
 json_object *golden=background_graph();const char *text=json_object_to_json_string(golden);
 assert(text&&strlen(text)<sizeof(background_golden));strcpy(background_golden,text);
 json_object_put(golden);healthy_stats();
 for(unsigned owned=0;owned<2;owned++){
  setup(0);capacity=28364+libc_baseline;peak=fail_size=fail_phase=0;owned_background=owned;
  json_object *o=background_graph();assert(o);
  if(owned)direct_json(nested_status,"test","average",o,false);
  else {const char *wire=json_object_to_json_string(o);assert(wire);nested_status("test/average",wire,false);json_object_put(o);}
  assert(nested_ok==owned);healthy_stats();
  char report[160];int n=snprintf(report,sizeof(report),"background %s actual-average+nested-full capacity=28364 peak=%u guard=%u full=%u live=0\n",owned?"owned":"legacy",(unsigned)(peak-libc_baseline),(unsigned)fail_size,nested_ok);_write(1,report,n);
 }
 return 0;
}
'''


class JsonBudget(unittest.TestCase):
    def test_arm32_background_overlap(self):
        self.compile_arm('A1', background_fixture(), budget=True)

    def test_arm32_actual_status_budget(self):
        self.compile_arm('A1', status_fixture(), budget=True)

    def test_owned_json_all_sdk_arm(self):
        for model in ('A1','M1','TC1'):
            with self.subTest(model=model):
                self.compile_arm(model, (ROOT/'tests/http_core/json_owned.c').read_text(), budget=False)

    def compile_arm(self, model, fixture, budget):
        sdk = Path(os.environ.get('PHICOMM_'+model+'_SDK', ROOT/'build'/('tc1-sdk-3.6.4' if model=='TC1' else model.lower()+'-sdk')/'mico-os'))
        cc = Path(os.environ.get('ARM_CC', ROOT/'build/tc1-tools/gcc-arm-none-eabi-5_4-2016q2/bin/arm-none-eabi-gcc'))
        qemu = shutil.which('qemu-arm') or shutil.which('qemu-arm-static')
        if not cc.exists() or not (sdk/'include/mico.h').exists() or not qemu:
            self.skipTest(model+' real SDK, pinned compiler and QEMU required')
        tc1 = model=='TC1'
        mcu, board = ('MW3xx','MK3031') if tc1 else ('MX1290','MK3080B')
        includes = ['include','platform','platform/include','platform/Cortex-M4',
            'platform/Cortex-M4/CMSIS','platform/MCU/'+mcu,'platform/MCU/'+mcu+'/peripherals',
            'platform/MCU/'+mcu+'/interface','board/'+board,'MiCO/system','MiCO/system/include',
            'libraries/utilities','MiCO/security','MiCO/security/TLS/mocSSL','libraries/utilities/json_c']
        if tc1:
            includes += ['platform/MCU/MW3xx/peripherals/sdk/src/incl/'+p for p in
                ('sdk','platform/os/micortos','sdk/drivers','sdk/drivers/mw300','sdk/drivers/mw300/regs')]
        flags = ['-std=gnu11','-g','-Os','-mthumb','-mcpu=cortex-m3' if tc1 else '-mcpu=cortex-m4',
            '-DMOC=1','-DMW310' if tc1 else '-DCONFIG_CPU_MX1290','-I'+str(ROOT),'-I'+str(ROOT/model)]
        if tc1:
            flags += ['-DCONFIG_VARIANT_MW310']
        else:
            flags += ['-mfpu=fpv4-sp-d16','-mfloat-abi=hard']
        flags += ['-I'+str(sdk/p) for p in includes]
        flags += ['-u','_printf_float','-u','_scanf_float','-nostartfiles','--specs=nano.specs',
            '--specs=nosys.specs','-Wl,-e,_start,-Ttext=0x10000']
        if budget:
            flags += ['-Wl,--wrap='+s for s in ('malloc','calloc','realloc','free','strdup','_malloc_r','_calloc_r','_realloc_r','_free_r')]
        with tempfile.TemporaryDirectory() as tmp:
            json=copy_patched_json(sdk/'libraries/utilities/json_c',Path(tmp)/'sdk')
            flags.insert(0,'-I'+str(json))
            retained = os.environ.get('PHICOMM_HTTP_JSON_ARTIFACT_DIR')
            directory = Path(retained).resolve()/('budget' if budget else model) if retained else Path(tmp)
            directory.mkdir(parents=True,exist_ok=True)
            source = directory/'probe.c';source.write_text(fixture)
            binary = directory/'probe.elf'
            obj=directory/'json_owned.o'
            subprocess.run([str(cc),*flags,'-fstack-usage','-c',str(ROOT/'common/json_owned.c'),'-o',str(obj)],check=True,timeout=30)
            json_objects=[]
            for path in json.glob('*.c'):
                target=directory/(path.stem+'.o');json_objects.append(str(target))
                subprocess.run([str(cc),*flags,'-fstack-usage','-c',str(path),'-o',str(target)],check=True,timeout=30)
            subprocess.run([str(cc),*flags,str(source),str(ROOT/'tests/http_core/json_arm_runtime.c'),
                str(ROOT/'common/httpd/product.c'),str(obj),*json_objects,
                '-o',str(binary)],check=True,timeout=30)
            subprocess.run([qemu,str(binary)],check=True,timeout=30)


if __name__=='__main__':
    unittest.main()

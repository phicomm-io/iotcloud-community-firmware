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
    fixture = (ROOT/'tests/http_core/status_budget.c').read_text()
    return fixture.replace('/* PRODUCTION_WATCHDOG_FILTER */', snapshots+watchdog+'\n'+filter_fn).replace('/* PRODUCTION_BUILDERS */', builders).replace('/* PRODUCTION_RESULT_QUERY */', result+'\n'+query)


class JsonBudget(unittest.TestCase):
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
            retained = os.environ.get('PHICOMM_HTTP_JSON_ARTIFACT_DIR')
            directory = Path(retained).resolve()/('budget' if budget else model) if retained else Path(tmp)
            directory.mkdir(parents=True,exist_ok=True)
            source = directory/'probe.c';source.write_text(fixture)
            binary = directory/'probe.elf'
            obj=directory/'json_owned.o'
            subprocess.run([str(cc),*flags,'-fstack-usage','-c',str(ROOT/'common/json_owned.c'),'-o',str(obj)],check=True,timeout=30)
            json_objects=[]
            for path in (sdk/'libraries/utilities/json_c').glob('*.c'):
                target=directory/(path.stem+'.o');json_objects.append(str(target))
                subprocess.run([str(cc),*flags,'-fstack-usage','-c',str(path),'-o',str(target)],check=True,timeout=30)
            subprocess.run([str(cc),*flags,str(source),str(ROOT/'tests/http_core/json_arm_runtime.c'),
                str(ROOT/'common/httpd/product.c'),str(obj),*json_objects,
                '-o',str(binary)],check=True,timeout=30)
            subprocess.run([qemu,str(binary)],check=True,timeout=30)


if __name__=='__main__':
    unittest.main()

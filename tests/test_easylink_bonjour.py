"""Actual pinned SDK Bonjour builder and publication boundary regression."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get('PHICOMM_TC1_SDK', os.environ.get('TC1_SDK_DIR', ROOT/'build/tc1-sdk-3.6.4/mico-os')))
REL = 'MiCO/system/easylink/internal/easylink_bonjour.c'

def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

HEADER = r'''
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
typedef int OSStatus;
typedef enum {Station=0,Soft_AP=1} WiFi_Interface;
typedef struct {struct {char rf_version[800],mac[20];} micoStatus;} system_context_t;
typedef struct {int unused;} net_para_st;
typedef struct {char *service_name,*host_name,*instance_name,*txt_record;int service_port;} mdns_init_t;
enum {kNoErr=0,kSizeErr=-6743,kNoMemoryErr=-6728};
static char fw[800],hardware[800],model_storage[800],manufacturer[800];
static char *model = model_storage;
#define FIRMWARE_REVISION fw
#define HARDWARE_REVISION hardware
#define PROTOCOL "1.0"
#define MODEL model
#define MANUFACTURER manufacturer
#define MicoGetVer() "3.6.4"
#define MICO_CONFIG_SERVER_PORT 8000
#define require_action(C,L,A) do {if(!(C)){A;goto L;}}while(0)
#define require_noerr(C,L) do {if(C)goto L;}while(0)
#define __strdup duplicate
static void micoWlanGetIPStatus(net_para_st*p,WiFi_Interface i){(void)p;(void)i;}
static void mdns_add_record(mdns_init_t,WiFi_Interface,int);
static void mdns_update_txt_record(const char*,WiFi_Interface,char*);
'''

class Bonjour(unittest.TestCase):
    def test_real_legacy_and_new_builder_and_publishers(self):
        # Pinned stock bytes are read from the SDK object database, so a local
        # already-overlaid SDK does not silently become its own oracle.
        old = subprocess.check_output(['git','-C',str(SDK),'show','HEAD:'+REL],text=True)
        escaping = subprocess.check_output(['git','-C',str(SDK),'show','HEAD:libraries/utilities/StringUtils.c'],text=True)
        with tempfile.TemporaryDirectory(prefix='bonjour-') as td:
            build=Path(td);target=build/REL;target.parent.mkdir(parents=True);target.write_text(old)
            for _ in range(2):
                subprocess.run(['python3',str(ROOT/'tools/apply_sdk_patch.py'),str(build),str(ROOT/'common/easylink-bonjour-bounds.patch')],check=True)
            new=target.read_text();self.assertNotIn('sprintf( txt_record, "%s',new)
            (build/'production.inc').write_text(new[new.index('/* Append escaped'):new.index('void easylink_remove_bonjour')])
            # Only adapt the ARM32 printf argument type for a 64-bit host; all
            # old format strings, escaping helper and order are SDK originals.
            legacy=function(old,'static void _bonjour_generate_txt_record(').replace('txt_record, easyLink_id','txt_record, (unsigned long)easyLink_id')
            (build/'legacy_builder.inc').write_text(legacy)
            (build/'legacy_escape.inc').write_text(function(escaping,'char *__strdup_trans_dot('))
            (build/'mico.h').write_text(HEADER)
            for config in (False,True):
                executable=build/('server' if config else 'native')
                subprocess.run([os.environ.get('CC','cc'),'-std=gnu99','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-include',str(build/'mico.h'),'-I'+str(build),*(['-DMICO_CONFIG_SERVER_ENABLE'] if config else []),str(ROOT/'tests/bonjour/fixture.c'),'-o',str(executable)],check=True)
                subprocess.run([str(executable)],check=True)

    def test_three_product_overlay_and_tc1_prototype(self):
        for model in ('A1','M1','TC1'):
            source=(ROOT/model/'build.sh').read_text()
            self.assertIn('common/easylink-bonjour-bounds.patch',source)
        self.assertIn('#include "status_guard.h"',(ROOT/'TC1/service.c').read_text())

if __name__=='__main__':unittest.main()

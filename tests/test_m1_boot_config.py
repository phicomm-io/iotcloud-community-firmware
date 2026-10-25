"""Execute M1's boot layout gate without allowing foreign config migrations."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class M1BootConfigTest(unittest.TestCase):
    def test_foreign_layout_reset_and_known_versions_preserved(self):
        source = (ROOT / 'M1/main.c').read_text()
        start = source.index('    if (app_config->magic != M1_CONFIG_MAGIC')
        end = source.index('    bool migration_changed', start)
        harness = r'''
#include "M1/storage.h"
#include <assert.h>
#include <stdio.h>
static app_storage_t storage;
static m1_config_t *app_config = &storage.base;
static unsigned resets, events;
static bool config_recovery;
static void appRestoreDefault_callback(void *data,uint32_t n) {
    assert(data==&storage && n==sizeof(storage));
    for(unsigned i=0;i<n;i++)assert(((unsigned char *)data)[i]==0);
    resets++;
    app_config->magic=M1_CONFIG_MAGIC;app_config->version=M1_CONFIG_VERSION;
    storage.co2_source=(m1_co2_source_config_t){.magic=M1_CO2_SOURCE_MAGIC};
    m1_display_defaults(&storage.display);
}
void service_event(const char *level,const char *domain,const char *event) {
    assert(!strcmp(domain,"config"));events++;
    assert(!strcmp(level,"error") && !strcmp(event,"unsupported_layout"));
}
static int gate(void) { GATE return 0; }
int main(void) {
    for(unsigned v=0;v<=M1_CONFIG_VERSION+1;v++) {
        memset(&storage,0xa5,sizeof(storage));
        app_config->magic=M1_CONFIG_MAGIC;app_config->version=v;
        storage.co2_source=(m1_co2_source_config_t){.magic=M1_CO2_SOURCE_MAGIC};
        m1_display_defaults(&storage.display);
        app_storage_t before=storage;resets=events=0;config_recovery=false;
        assert(gate()==0);
        bool unknown=v<1 || v>M1_CONFIG_VERSION;
        assert(config_recovery==unknown && resets==unknown && events==unknown);
        if(!unknown)assert(!memcmp(&before,&storage,sizeof(storage)));
        else assert(app_config->magic==M1_CONFIG_MAGIC && app_config->version==M1_CONFIG_VERSION);
    }
    memset(&storage,0xa5,sizeof(storage));resets=events=0;config_recovery=false;
    assert(gate()==0 && config_recovery && resets==1 && events==1);
    for(unsigned kind=0;kind<7;kind++) {
        memset(&storage,0,sizeof(storage));app_config->magic=M1_CONFIG_MAGIC;app_config->version=M1_CONFIG_VERSION;
        storage.co2_source=(m1_co2_source_config_t){.magic=M1_CO2_SOURCE_MAGIC};
        m1_display_defaults(&storage.display);
        if(kind==0)storage.co2_source.magic=0;
        if(kind==1)memset(storage.co2_source.device_id,'a',13);
        if(kind==2)storage.co2_source.external=2;
        if(kind==3)storage.co2_source.double_click=2;
        if(kind==4)storage.co2_source.reserved[0]=1;
        if(kind==5)storage.co2_source.revision=UINT32_MAX;
        if(kind==6)storage.co2_source.device_id[0]='a';
        resets=events=0;config_recovery=false;
        assert(gate()==0 && config_recovery && resets==1 && events==1);
    }
    puts("M1 boot: known layouts preserved, unsupported layouts enter RAM-only recovery: OK");
}
'''.replace('GATE', source[start:end])
        with tempfile.TemporaryDirectory() as temporary:
            probe, binary = Path(temporary) / 'probe.c', Path(temporary) / 'probe'
            probe.write_text(harness)
            command = [os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra', '-Werror',
                       '-I'+str(ROOT), '-I'+str(ROOT/'tests/host'), '-I'+str(ROOT/'M1'),
                       '-I'+str(ROOT/'build/m1-sdk/mico-os/libraries/utilities/json_c'), str(probe), '-o', str(binary)]
            if os.environ.get('SANITIZE') == '1':
                command += ['-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined']
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True)

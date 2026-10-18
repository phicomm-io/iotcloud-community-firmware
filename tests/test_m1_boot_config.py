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
        start = source.index('    if (app_config->magic != M1_CONFIG_MAGIC)')
        end = source.index('    if (app_config->magic == M1_CONFIG_MAGIC &&', start)
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#define M1_CONFIG_MAGIC 0x4d314346u
#define M1_CONFIG_VERSION 5
#define kNoErr 0
#define kUnknownErr -1
typedef struct { uint32_t magic,version; char credentials[64]; } config_t;
typedef struct { config_t base; unsigned char extensions[300]; } app_storage_t;
static app_storage_t storage;
static config_t *app_config = &storage.base;
static struct { char wifi[32]; } context = {"saved SDK credentials"};
static void *m1_context = &context;
static unsigned resets, writes, events;
static bool fail_write;
static void appRestoreDefault_callback(void *data,uint32_t n) {
    assert(data==&storage && n==sizeof(storage));
    for(unsigned i=0;i<n;i++)assert(((unsigned char *)data)[i]==0);
    resets++;
    app_config->magic=M1_CONFIG_MAGIC;app_config->version=M1_CONFIG_VERSION;
}
static int mico_system_context_update(void *ctx) {
    assert(ctx==&context && !strcmp(context.wifi,"saved SDK credentials"));
    writes++;return fail_write?-1:0;
}
static void service_event(const char *level,const char *domain,const char *event) {
    assert(!strcmp(domain,"config"));events++;
    assert(!strcmp(event,!strcmp(level,"info")?"foreign_layout_reset":"unsupported_layout"));
}
static int gate(void) { GATE return 0; }
int main(void) {
    for(unsigned v=0;v<=M1_CONFIG_VERSION+1;v++) {
        memset(&storage,0xa5,sizeof(storage));
        app_config->magic=M1_CONFIG_MAGIC;app_config->version=v;
        app_storage_t before=storage;
        resets=writes=events=0;fail_write=false;
        assert(gate()==((v<1 || v>M1_CONFIG_VERSION)?-1:0));
        assert(!resets && !writes && !memcmp(&before,&storage,sizeof(storage)));
        assert(events==(v<1 || v>M1_CONFIG_VERSION));
    }
    for(unsigned failure=0;failure<2;failure++) {
        memset(&storage,0xa5,sizeof(storage));resets=writes=events=0;
        fail_write=failure;
        assert(gate()==(failure?-1:0));
        assert(resets==1 && writes==1 && events==!failure);
        assert(app_config->magic==M1_CONFIG_MAGIC && app_config->version==M1_CONFIG_VERSION);
    }
    puts("M1 boot: reset foreign app data, preserve known layouts/SDK credentials, fail on persist error: OK");
}
'''.replace('GATE', source[start:end])
        with tempfile.TemporaryDirectory() as temporary:
            probe, binary = Path(temporary) / 'probe.c', Path(temporary) / 'probe'
            probe.write_text(harness)
            command = [os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra', '-Werror',
                       str(probe), '-o', str(binary)]
            if os.environ.get('SANITIZE') == '1':
                command += ['-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined']
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True)

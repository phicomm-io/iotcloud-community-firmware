"""Actual product cleanup must pass the length-aware binary callback."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tests.test_http_json_budget import function

ROOT = Path(__file__).resolve().parents[1]


class M1CleanupCallback(unittest.TestCase):
    def test_actual_cleanup_binary_callback_and_errors(self):
        cleanup = function((ROOT / 'M1/mqtt.c').read_text(), 'static int cleanup_all(')
        fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#define MQTT_SUCCESS 0
#define MQTT_FAILURE -1
static int discovery_rc, feature_rc;
static unsigned calls;
static int publish_bytes(const char *topic,const void *bytes,size_t length,bool retain){
 assert(!strcmp(topic,"test/topic")&&length==3&&retain);
 assert(!memcmp(bytes,"a\0b",3));calls++;return 0;
}
static int cleanup_discovery(void){return discovery_rc;}
static int feature_mqtt_cleanup(int (*sender)(const char*,const void*,size_t,bool)){
 assert(sender==publish_bytes);assert(!sender("test/topic","a\0b",3,true));return feature_rc;
}
/* PRODUCT */
int main(void){
 for(unsigned a=0;a<2;a++)for(unsigned b=0;b<2;b++){
  discovery_rc=a?-1:0;feature_rc=b?-1:0;
  assert(cleanup_all()==(a||b?-1:0));
 }
 assert(calls==4);return 0;
}
'''.replace('/* PRODUCT */', cleanup)
        # Both success and failure still execute cleanup with binary lengths.
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / 'probe.c').write_text(fixture)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                            str(path / 'probe.c'), '-o', str(path / 'probe')], check=True)
            subprocess.run([str(path / 'probe')], check=True)

"""Round-trip TC1's factory MQTT configuration through production validation."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.sdk_json import copy_patched_json
from tests.test_tc1_mqtt_stack import extract

ROOT = Path(__file__).resolve().parents[1]


class TC1MqttConfig(unittest.TestCase):
    def test_unconfigured_restore_and_invalid_active_ports(self):
        service = (ROOT/'TC1/service.c').read_text()
        helpers = '\n'.join(extract(service, signature) for signature in (
            'static bool keys(', 'static bool boolparam(',
            'static bool number(', 'static bool strparam('))
        branch = extract(service, 'else if(!strcmp(s,"mqtt"))')
        sdk = Path(os.environ.get('TC1_SDK_DIR', ROOT/'build/tc1-sdk-3.6.4/mico-os'))
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)
            json = copy_patched_json(sdk/'libraries/utilities/json_c', out/'sdk')
            for source in json.glob('*.c'):
                source.write_text(source.read_text().replace('%ld', '%d'))
            fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "json.h"
static struct {
    char mqtt_host[64], mqtt_user[64], mqtt_password[257];
    int mqtt_port, mqtt_topic_mode;
    struct { bool mqtt_enabled; unsigned revision; } bemfa;
} config;
#define user_config (&config)
static unsigned locks;
static void tc1_status_lock(void) { assert(!locks); locks++; }
static void tc1_status_unlock(void) { assert(locks == 1); locks--; }
''' + helpers + r'''
static bool apply(const char *wire) {
    json_object *v = json_tokener_parse(wire);
    assert(v);
    bool valid = false;
    const char *s = "mqtt";
    if (0) {} BRANCH
    json_object_put(v);
    assert(!locks);
    return valid;
}
int main(void) {
    config.mqtt_topic_mode = 1;
    config.bemfa.mqtt_enabled = true;
    assert(apply("{\"host\":\"lab\",\"port\":18884,\"user\":\"test\",\"password\":\"temporary\",\"enabled\":true}"));
    assert(!strcmp(config.mqtt_host,"lab") && config.mqtt_port == 18884);
    assert(apply("{\"host\":\"\",\"port\":0,\"user\":\"\",\"password\":\"\",\"protocol\":\"tmcp\",\"enabled\":true}"));
    assert(!config.mqtt_host[0] && !config.mqtt_user[0] && !config.mqtt_password[0]);
    assert(!config.mqtt_port && config.mqtt_topic_mode == 1 && config.bemfa.mqtt_enabled);
    assert(config.bemfa.revision == 2);
    unsigned char before[sizeof(config)];
    memcpy(before, &config, sizeof(config));
    const char *invalid[] = {
        "{\"host\":\"broker\",\"port\":0,\"user\":\"\"}",
        "{\"host\":\"broker\",\"port\":0,\"user\":\"\",\"enabled\":false}",
        "{\"host\":\"\",\"port\":-1,\"user\":\"\"}",
        "{\"host\":\"broker\",\"port\":65536,\"user\":\"\"}",
        "{\"host\":\"\",\"port\":\"0\",\"user\":\"\"}",
        "{\"host\":\"\",\"port\":false,\"user\":\"\"}",
        "{\"host\":\"\",\"user\":\"\"}",
        "{\"host\":\"\",\"port\":0,\"user\":\"\",\"protocol\":\"invalid\"}",
        "{\"host\":\"\",\"port\":0,\"user\":\"\",\"unknown\":true}"
    };
    for (unsigned i=0; i<sizeof(invalid)/sizeof(*invalid); i++) {
        assert(!apply(invalid[i]));
        assert(!memcmp(before, &config, sizeof(config)));
    }
    assert(apply("{\"host\":\"broker\",\"port\":1,\"user\":\"\",\"password\":\"kept\",\"protocol\":\"legacy\",\"enabled\":false}"));
    assert(config.mqtt_port == 1 && !config.mqtt_topic_mode && !config.bemfa.mqtt_enabled);
    assert(apply("{\"host\":\"broker\",\"port\":65535,\"user\":\"\"}"));
    assert(config.mqtt_port == 65535 && !strcmp(config.mqtt_password,"kept"));
    puts("PASS TC1 factory MQTT restore, invalid ports and omitted secret preservation");
    return 0;
}
'''.replace('BRANCH', branch)
            (out/'probe.c').write_text(fixture)
            command = [os.environ.get('CC', 'cc'), '-std=gnu99', '-g',
                       '-Wall', '-Wextra', '-Wno-misleading-indentation',
                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                       '-I'+str(ROOT/'tests/stubs'), '-I'+str(ROOT), '-I'+str(json), str(out/'probe.c'),
                       *map(str, sorted(json.glob('*.c'))), '-o', str(out/'probe')]
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(out/'probe')], capture_output=True, text=True,
                                    timeout=10, env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1'})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())


if __name__ == '__main__':
    unittest.main()

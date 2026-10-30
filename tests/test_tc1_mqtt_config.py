"""Factory MQTT restore through the production TC1 borrowed MP validator."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from tests.test_tc1_mqtt_stack import extract

ROOT = Path(__file__).resolve().parents[1]


def pack(value):
    """Independent MessagePack fixtures for the few types used by config.set."""
    if isinstance(value, bool):
        return b'\xc3' if value else b'\xc2'
    if isinstance(value, int):
        return b'\xd2' + struct.pack('>i', value)
    if isinstance(value, str):
        data = value.encode()
        return b'\xda' + struct.pack('>H', len(data)) + data
    if isinstance(value, dict):
        return b'\xde' + struct.pack('>H', len(value)) + b''.join(
            pack(key) + pack(item) for key, item in value.items())
    raise TypeError(type(value))


class TC1MqttConfig(unittest.TestCase):
    def test_unconfigured_restore_and_invalid_active_ports(self):
        service = (ROOT/'TC1/service_mp.inc').read_text()
        helpers = '\n'.join(extract(service, signature) for signature in (
            'static bool tc1_mp_has(', 'static bool keys(',
            'static bool boolparam(', 'static bool number(',
            'static bool strparam('))
        branch = extract(service, 'else if(!strcmp(s,"mqtt"))')
        initial = dict(host='lab', port=18884, user='test',
                       password='temporary', enabled=True)
        factory = dict(host='', port=0, user='', password='',
                       protocol='tmcp', enabled=True)
        invalid = [
            dict(host='broker', port=0, user=''),
            dict(host='broker', port=0, user='', enabled=False),
            dict(host='', port=-1, user=''),
            dict(host='broker', port=65536, user=''),
            dict(host='', port='0', user=''),
            dict(host='', port=False, user=''),
            dict(host='', user=''),
            dict(host='', port=0, user='', protocol='invalid'),
            dict(host='', port=0, user='', protocol='legacy'),
            dict(host='', port=0, user='', unknown=True),
        ]
        cases = [initial, factory, *invalid,
                 dict(host='broker', port=1, user='', password='kept',
                      protocol='tmcp', enabled=False),
                 dict(host='broker', port=65535, user='')]
        encoded = [pack(case) for case in cases]
        vectors = '\n'.join(
            'static const unsigned char case_%d[]={%s};' %
            (index, ','.join(str(byte) for byte in data))
            for index, data in enumerate(encoded))
        calls = '\n'.join(
            'assert(!apply(case_%d,sizeof(case_%d)));'
            'assert(!memcmp(before,&config,sizeof(config)));' % (index, index)
            for index in range(2, 2+len(invalid)))
        fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "common/typed_wire.h"
static struct {
    char mqtt_host[64], mqtt_user[64], mqtt_password[257];
    int mqtt_port, mqtt_topic_mode;
    struct { bool mqtt_enabled; unsigned revision; } bemfa;
} config;
#define user_config (&config)
static unsigned locks;
static void tc1_status_lock(void) { assert(!locks); locks++; }
static void tc1_status_unlock(void) { assert(locks == 1); locks--; }
HELPERS
static bool apply(const void *wire,size_t length) {
    ph_value values;
    assert(phv_parse(PH_WIRE_MSGPACK,wire,length,&values));
    const ph_value *v=&values;
    bool valid = false;
    const char *s = "mqtt";
    if (0) {} BRANCH
    assert(!locks);
    return valid;
}
VECTORS
int main(void) {
    config.mqtt_topic_mode = 1;
    config.bemfa.mqtt_enabled = true;
    assert(apply(case_0,sizeof(case_0)));
    assert(!strcmp(config.mqtt_host,"lab") && config.mqtt_port == 18884);
    assert(apply(case_1,sizeof(case_1)));
    assert(!config.mqtt_host[0] && !config.mqtt_user[0] && !config.mqtt_password[0]);
    assert(!config.mqtt_port && config.mqtt_topic_mode == 1 && config.bemfa.mqtt_enabled);
    assert(config.bemfa.revision == 2);
    unsigned char before[sizeof(config)];
    memcpy(before, &config, sizeof(config));
INVALID_CALLS
    assert(apply(case_12,sizeof(case_12)));
    assert(config.mqtt_port == 1 && config.mqtt_topic_mode && !config.bemfa.mqtt_enabled);
    assert(apply(case_13,sizeof(case_13)));
    assert(config.mqtt_port == 65535 && !strcmp(config.mqtt_password,"kept"));
    puts("PASS TC1 MP MQTT factory restore, invalid ports/types, binary-only protocol and omitted secret preservation");
    return 0;
}
'''.replace('HELPERS', helpers).replace('BRANCH', branch).replace(
            'VECTORS', vectors).replace('INVALID_CALLS', calls)
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)
            (out/'probe.c').write_text(fixture)
            command = [os.environ.get('CC', 'cc'), '-std=gnu99', '-g',
                       '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation',
                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                       '-I'+str(ROOT), str(out/'probe.c'),
                       str(ROOT/'common/typed_wire.c'), str(ROOT/'common/wire_buffer.c'),
                       '-lm', '-o', str(out/'probe')]
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(out/'probe')], capture_output=True, text=True,
                                    timeout=10, env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1'})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())


if __name__ == '__main__':
    unittest.main()

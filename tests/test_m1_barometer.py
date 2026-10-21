import os
from pathlib import Path
import subprocess
from tests.sdk_json import copy_patched_json
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class M1BarometerTest(unittest.TestCase):
    def test_production_driver_with_mock_i2c(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / 'barometer'
            command = [os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra', '-Werror',
                       '-DBMP3_64BIT_COMPENSATION=1', '-Itests/barometer', '-IM1',
                       'tests/test_m1_barometer.c', 'M1/barometer.c', 'M1/bmp3_vendor/bmp3.c',
                       '-o', str(binary)]
            if os.environ.get('SANITIZE') == '1':
                command += ['-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined', '-fno-pie', '-no-pie']
            subprocess.run(command, cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True)

    def test_status_json_identity_units_and_expiry(self):
        source = (ROOT / 'common/device_status.c').read_text()
        helpers = source[source.index('static void status_string('):source.index('static const char *device_mqtt_status(')]
        fields = source[source.index('    json_object *barometer ='):source.index('    status_int(o, "brightness_requested"')]
        harness = r'''
#include <assert.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include "json.h"
#include "common/json_build.h"
#include "M1/barometer.h"
HELPERS
static json_object *snapshot(barometer_state_t state,uint32_t now) {
    struct { struct { struct { barometer_state_t barometer; } m1; } sensor; } frozen={{{state}}}, *snapshot=&frozen;
    json_object *o=json_object_new_object();
    FIELDS
    json_object *result=json_object_object_get(o,"barometer");json_object_get(result);json_object_put(o);return result;
}
int main(void) {
    barometer_state_t state={0};json_object *b=snapshot(state,0);
    assert(!json_object_object_get(b,"model") && !json_object_object_get(b,"chip_id"));
    assert(!json_object_object_get(b,"address") && !json_object_object_get(b,"pressure_hpa"));
    assert(!json_object_object_get(b,"age_ms"));json_object_put(b);
    state=(barometer_state_t){.ready=true,.received=true,.chip_id=0x50,.address=0x77,.pressure_centi_pa=10132400};
    b=snapshot(state,9999);
    assert(!strcmp(json_object_get_string(json_object_object_get(b,"model")),"BMP388"));
    assert(json_object_get_int(json_object_object_get(b,"chip_id"))==80);
    assert(json_object_get_int(json_object_object_get(b,"address"))==119);
    assert(fabs(json_object_get_double(json_object_object_get(b,"pressure_hpa"))-1013.24)<0.000001);
    assert(json_object_get_boolean(json_object_object_get(b,"valid")));json_object_put(b);
    b=snapshot(state,10000);assert(!json_object_object_get(b,"pressure_hpa"));
    assert(!json_object_get_boolean(json_object_object_get(b,"valid")));json_object_put(b);
    state.chip_id=0x60;state.address=0x76;b=snapshot(state,1);
    assert(!strcmp(json_object_get_string(json_object_object_get(b,"model")),"BMP390"));json_object_put(b);
    state.ready=false;b=snapshot(state,1);assert(!json_object_object_get(b,"pressure_hpa"));json_object_put(b);
}
'''.replace('HELPERS', helpers).replace('FIELDS', fields)
        json_dir = Path(os.environ.get('PHICOMM_TEST_JSON_DIR', ROOT / 'build/a1-sdk/mico-os/libraries/utilities/json_c'))
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            copied = copy_patched_json(json_dir, directory / 'sdk')
            # MiCO json-c assumes 32-bit long; adapt only the disposable host copy.
            for file in copied.glob('*.c'):
                file.write_text(file.read_text().replace('%ld', '%d'))
            probe = directory / 'status.c'
            probe.write_text(harness)
            binary = directory / 'status'
            command = [os.environ.get('CC', 'cc'), '-std=gnu99', '-I'+str(ROOT), '-I'+str(copied),
                            '-I'+str(ROOT/'tests/host'), '-I'+str(ROOT/'tests/stubs'),
                            str(probe), *map(str,copied.glob('*.c')), '-lm', '-o', str(binary)]
            if os.environ.get('SANITIZE') == '1':
                command += ['-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined', '-fno-pie', '-no-pie']
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True)

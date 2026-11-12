#!/usr/bin/env python3
"""Validate HA state with the pinned framework's actual float formatter."""
import argparse
import json
import math
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app-source', type=Path, required=True)
    parser.add_argument('--run-dir', type=Path, required=True)
    args = parser.parse_args()
    run = args.run_dir.resolve()
    run.mkdir(parents=True, exist_ok=True)
    native = (args.app_source/'src/new_common.c').read_text()
    config = '\n'.join(line for line in native.splitlines()
                       if line.startswith('#define NANOPRINTF_'))
    port = (Path(__file__).resolve().parents[1]/'src/ph_mqtt_port.c').read_text()
    start = port.index('static size_t ha_state(')
    end = port.index('static int publish_cycle(', start)
    source = run/'ha-numbers.c'
    source.write_text('''#include <stdio.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>
'''+config+'''
#include "nanoprintf.h"
#define snprintf npf_snprintf
#include "ha_number.h"
static unsigned selected;
static void bl_lock(void){}
static void bl_unlock(void){}
static unsigned ph_os_now(void){return 0;}
static unsigned ph_meter_fields(void *unused,unsigned now){(void)unused;(void)now;return selected;}
static struct {bool relay;char version[64];struct {double value[3],energy_kwh;} meter;} bl_device;
'''+port[start:end]+'''
static void render(unsigned fields,double v,double i,double p,double energy){
 selected=fields;bl_device.meter.value[0]=v;bl_device.meter.value[1]=i;bl_device.meter.value[2]=p;bl_device.meter.energy_kwh=energy;
 strcpy(bl_device.version,"host-nanoprintf");char out[4096];size_t n=ha_state(out,sizeof(out));if(n)puts(out);
}
int main(void){
 render(0,0,0,0,0);render(7,230.125,.123456,12.5,1.234567);
 render(3,0,0,0,0);render(7,500,100,25000,1e12);render(7,230,.000001,.00001,.000001);
 render(7,NAN,100,1,NAN);render(7,230,.1,10,1.9999996);render(0,0,0,0,-1);render(0,0,0,0,1000000000001.0);return 0;
}
''')
    executable = run/'ha-numbers'
    subprocess.run(['clang', '-std=c99', '-g', '-O1', '-Wall', '-Wextra',
                    '-Wno-unused-function', '-fsanitize=address,undefined',
                    '-I', str(args.app_source/'src'), '-I',
                    str(Path(__file__).resolve().parents[1]/'src'), str(source), '-lm',
                    '-o', str(executable)], check=True)
    result = subprocess.run([str(executable)], text=True, capture_output=True, timeout=5)
    (run/'test.log').write_text(result.stdout+result.stderr)
    result.check_returncode()
    states = [json.loads(line) for line in result.stdout.splitlines()]
    assert len(states) == 9
    assert states[0]['energy_kwh'] == 0 and states[0]['voltage'] is None
    assert states[1]['voltage'] == 230.125
    assert abs(states[1]['current']-.123456) < .000002
    assert abs(states[1]['energy_kwh']-1.234567) < .000002
    assert states[2]['voltage'] == 0 and states[2]['power_watts'] is None
    assert states[3]['energy_kwh'] == 1e12
    assert states[4]['energy_kwh'] == .000001
    assert states[5]['voltage'] is None and states[5]['energy_kwh'] is None
    assert states[6]['energy_kwh'] == 2
    assert states[7]['energy_kwh'] is None and states[8]['energy_kwh'] is None
    assert all(v is None or not isinstance(v, (int, float)) or math.isfinite(v)
               for state in states for v in state.values())
    (run/'result.json').write_text(json.dumps({
        'status': 'PASS', 'production_ha_state_format_verbatim': True,
        'framework_nanoprintf': True, 'asan_ubsan': True,
        'states': states,
    }, indent=2)+'\n')
    print('PASS: HA JSON numbers with the actual framework formatter, missing samples, zero, fractions and upper limits')


if __name__ == '__main__':
    main()

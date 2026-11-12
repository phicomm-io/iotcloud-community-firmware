import os
from pathlib import Path
import subprocess
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
        # Keep the historical case name, but exercise the complete production
        # MP status writer. Missing readings are explicit nil in native output.
        harness = r'''
#include <assert.h>
#include <math.h>
#include <string.h>
#include "common/device_status.h"
static ph_buf *bytes;
static ph_value snapshot(barometer_state_t state,uint32_t now){
    device_status_t frozen={0};device_status_full_t full={0};
    frozen.model=DEVICE_M1;frozen.captured=63;frozen.now_ms=now;
    frozen.identity.version="1.0.0-rc.4";strcpy(frozen.identity.id,"aabbccddeeff");
    frozen.sensor.m1.barometer=state;full.watchdog.fault="";full.watchdog.reset_name="unknown";
    ph_writer writer;assert(phw_init(&writer,PH_WIRE_MSGPACK));device_status_mp(&writer,&frozen,&full);
    ph_buf_free(bytes);bytes=phw_take(&writer);assert(bytes);ph_value root,b;
    assert(phv_parse(PH_WIRE_MSGPACK,bytes->buf,bytes->len,&root));assert(phv_get(&root,"barometer",&b)&&b.kind==PHV_MAP);return b;
}
static ph_value get(ph_value b,const char *key){ph_value value;assert(phv_get(&b,key,&value));return value;}
static void nil(ph_value b,const char *key){assert(get(b,key).kind==PHV_NULL);}
static void text(ph_value b,const char *key,const char *expected){ph_value v=get(b,key);assert(phv_equal_text(&v,expected));}
static void integer(ph_value b,const char *key,int64_t expected){ph_value v=get(b,key);int64_t n;assert(phv_int(&v,&n)&&n==expected);}
static void valid(ph_value b,bool expected){ph_value v=get(b,"valid");bool flag;assert(phv_bool(&v,&flag)&&flag==expected);}
int main(void){
    barometer_state_t state={0};ph_value b=snapshot(state,0);
    nil(b,"model");assert(!phv_get(&b,"chip_id",&(ph_value){0}));assert(!phv_get(&b,"address",&(ph_value){0}));nil(b,"pressure_hpa");nil(b,"age_ms");
    state=(barometer_state_t){.ready=true,.received=true,.chip_id=0x50,.address=0x77,.pressure_centi_pa=10132400};
    b=snapshot(state,9999);text(b,"model","BMP388");assert(!phv_get(&b,"chip_id",&(ph_value){0}));assert(!phv_get(&b,"address",&(ph_value){0}));
    ph_value pressure=get(b,"pressure_hpa");double n;assert(phv_number(&pressure,&n)&&fabs(n-1013.24)<0.000001);valid(b,true);
    b=snapshot(state,10000);nil(b,"pressure_hpa");valid(b,false);
    state.chip_id=0x60;state.address=0x76;b=snapshot(state,1);text(b,"model","BMP390");
    state.ready=false;b=snapshot(state,1);nil(b,"pressure_hpa");
    ph_buf_free(bytes);return 0;
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            directory=Path(temporary);probe=directory/'status.c';probe.write_text(harness);binary=directory/'status'
            command=[os.environ.get('CC','cc'),'-std=gnu99','-DPHICOMM_APPLICATION_WATCHDOG=1','-I'+str(ROOT),str(probe),
                     *[str(ROOT/p) for p in ('common/device_status.c','common/typed_wire.c','common/wire_buffer.c')],'-lm','-o',str(binary)]
            if os.environ.get('SANITIZE')=='1':command+=['-g','-fno-omit-frame-pointer','-fsanitize=address,undefined','-fno-pie','-no-pie']
            subprocess.run(command,check=True);subprocess.run([str(binary)],check=True)

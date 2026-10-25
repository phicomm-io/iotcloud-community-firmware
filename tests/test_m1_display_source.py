"""Execute production M1 source selection, leases and UART button policy."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class DisplaySourceTest(unittest.TestCase):
    def test_production_source_cache_and_uart_owner(self):
        source = r'''
#include <assert.h>
#include <math.h>
#include "M1/m1.h"
#define MICO_UART_1 1
static uint32_t now=1000;
uint32_t mico_rtos_get_time(void){return now;}
void mico_thread_msleep(unsigned n){now+=n;}
char m1_id[13]="aabbccddeeff";
mico_mutex_t m1_lock;
m1_state_t m1_state;
static m1_co2_source_config_t settings={.magic=M1_CO2_SOURCE_MAGIC,.revision=1,.external=1,.double_click=1,.device_id="123456abcdef"};
static m1_co2_config_t display={.magic=M1_CO2_MAGIC,.revision=1,.mode=1,.interval=5,.low=1000,.high=1500};
void m1_co2_source_config_get(m1_co2_source_config_t *out){*out=settings;}
void m1_co2_config_get(m1_co2_config_t *out){*out=display;}
void m1_alarm_get(m1_alarm_config_t *out){memset(out,0,sizeof(*out));out->reserved[0]=M1_ALARM_SCHEMA;}
const char *m1_co2_source_config_save(const m1_co2_source_config_t *c){(void)c;return "unsupported";}
static unsigned alarm_stops;
void m1_wake_alarm_stop(void){alarm_stops++;m1_state.wake_alarm_active=false;}
static uint8_t sent[12];static unsigned sends;
static int MicoUartSend(unsigned port,const void *data,unsigned n){assert(port==1&&n==12);memcpy(sent,data,n);sends++;return 0;}
#include "M1/co2_source.c"
#include "M1/mcu.c"
static void reply(void){
    uint8_t p[20]={0x23,0x70,sent[2],sent[3],0};p[19]=0x21;
    if(p[2]==0){p[5]=2;p[6]=p[7]=255;p[14]=1;p[15]=15;p[16]=1;}
    m1_mcu_rx(p);
}
int main(void){
    assert(m1_co2_source_config_valid(&settings));
    m1_co2_source_config_t invalid=settings;invalid.device_id[4]='G';assert(!m1_co2_source_config_valid(&invalid));
    invalid=settings;invalid.external=0;assert(!m1_co2_source_config_valid(&invalid));
    uint16_t ppm,ttl;assert(!m1_co2_source_value(&ppm,&ttl)&&!ppm&&!ttl);
    feature_sample_t s={.boot=7,.sequence=1,.co2_valid=true,.co2=1234,.co2_age_ms=1000,.co2_received_ms=now};
    m1_co2_source_sample("ffffffffffff",&s);assert(!m1_co2_source_value(&ppm,&ttl));
    m1_co2_source_sample(settings.device_id,&s);assert(m1_co2_source_value(&ppm,&ttl)&&ppm==1234&&ttl==19);
    s.co2=999;m1_co2_source_sample(settings.device_id,&s);assert(m1_co2_source_value(&ppm,&ttl)&&ppm==1234);
    s.sequence=2;s.co2_valid=false;m1_co2_source_sample(settings.device_id,&s);assert(!m1_co2_source_value(&ppm,&ttl));
    s.sequence=3;s.co2_valid=true;s.co2=NAN;m1_co2_source_sample(settings.device_id,&s);assert(!m1_co2_source_value(&ppm,&ttl));
    s.sequence=4;s.co2=2500;m1_co2_source_sample(settings.device_id,&s);
    now+=18999;assert(m1_co2_source_value(&ppm,&ttl)&&ttl==1);now++;assert(!m1_co2_source_value(&ppm,&ttl));
    s.boot=8;s.sequence=1;s.co2_received_ms=now;m1_co2_source_sample(settings.device_id,&s);assert(m1_co2_source_value(&ppm,&ttl));
    s.boot=7;s.sequence=999;s.co2=111;m1_co2_source_sample(settings.device_id,&s);assert(m1_co2_source_value(&ppm,&ttl)&&ppm==2500);
    settings.revision++;assert(!m1_co2_source_value(&ppm,&ttl));m1_co2_source_reset();
    now=UINT32_MAX-500;s.boot=9;s.sequence=1;s.co2_received_ms=now;s.co2_age_ms=0;s.co2=1500;
    m1_co2_source_sample(settings.device_id,&s);now+=19999;assert(m1_co2_source_value(&ppm,&ttl)&&ttl==1);now++;assert(!m1_co2_source_value(&ppm,&ttl));
    settings.external=0;memset(settings.device_id,0,13);settings.revision++;
    m1_state.scd40.ready=m1_state.scd40.received=true;m1_state.scd40.co2_ppm=777;m1_state.scd40.updated_ms=now;
    assert(m1_co2_source_value(&ppm,&ttl)&&ppm==777&&ttl==20);
    now=1000;m1_state.scd40.updated_ms=now;strcpy(m1_state.scd40.serial,"sensor");
    m1_state.mcu_version_received=true;m1_mcu_tick();assert(sent[2]==0);reply();
    assert(m1_state.mcu_display_source&&m1_state.mcu_button_events);
    m1_mcu_tick();assert(sent[2]==17&&sent[4]==3&&sent[5]==9&&sent[6]==0&&sent[7]==2&&sent[8]==20);reply();
    m1_mcu_tick();assert(sent[2]==18&&sent[4]==1);reply();
    uint8_t button[20]={0x23,0x71,1,2,1};button[19]=0x21;
    m1_state.wake_alarm_active=true;assert(m1_button_rx(button)&&alarm_stops==1&&manual_display_mode<0);
    assert(m1_button_rx(button)&&alarm_stops==1);button[4]++;
    assert(m1_button_rx(button)&&manual_display_mode==0);
    button[4]++;assert(m1_button_rx(button)&&manual_display_mode==1);
    button[4]++;button[3]=1;assert(m1_button_rx(button)&&manual_display_mode==1);
    button[3]=2;button[4]++;m1_state.ota_active=true;assert(m1_button_rx(button)&&manual_display_mode==1);m1_state.ota_active=false;
    settings.double_click=0;button[4]++;assert(m1_button_rx(button)&&manual_display_mode==1);
    settings.double_click=1;m1_state.scd40.serial[0]=0;button[4]++;assert(m1_button_rx(button)&&manual_display_mode==1);
    /* Double-click cycles exactly two sources and never persists a setting. */
    uint8_t a[7];
    const unsigned values[]={999,1000,9999,10000,65535},numbers[]={999,100,999,100,655},decimals[]={0,1,1,2,2};
    for(unsigned i=0;i<5;i++){m1_display_source_encode(a,values[i],2,0,1000,1500);assert((unsigned)a[0]*256+a[1]==numbers[i]&&a[2]==decimals[i]&&a[4]==2&&!a[5]&&!a[6]);}
    assert(m1_display_source_selected(2,-1,4999,5)&&!m1_display_source_selected(2,-1,5000,5)&&m1_display_source_selected(2,-1,10000,5));
    assert(m1_display_source_selected(0,1,0,5)&&!m1_display_source_selected(1,0,0,5));
    assert(!m1_lock&&sends==3);return 0;
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            src,binary=Path(temporary)/'source.c',Path(temporary)/'source'
            src.write_text(source)
            json_dir=Path(os.environ.get('PHICOMM_TEST_JSON_DIR',ROOT/'build/m1-sdk/mico-os/libraries/utilities/json_c'))
            subprocess.run([os.environ.get('CC','cc'),'-std=gnu99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                            '-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                            '-I',str(ROOT/'tests/host'),'-I',str(ROOT),'-I',str(json_dir),str(src),str(ROOT/'M1/protocol.c'),'-Wl,--gc-sections','-lm','-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

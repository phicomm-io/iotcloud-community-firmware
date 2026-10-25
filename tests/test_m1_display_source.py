"""Execute production M1 source selection, leases and UART button policy."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class DisplaySourceTest(unittest.TestCase):
    def _dual_source(self):
        main = (ROOT / 'M1/main.c').read_text()
        production = main[main.index('void m1_co2_config_get'):main.index('static void alarm_load_locked')]
        source = r'''
#include <assert.h>
#include "M1/storage.h"
static app_storage_t live,flash;
static m1_config_t *app_config=&live.base;
static mico_mutex_t config_lock;
static bool config_recovery,fail_flash;
static unsigned writes,stops;
static uint32_t now=1000;
char m1_id[13]="aabbccddeeff";
mico_mutex_t m1_lock;
m1_state_t m1_state;
mico_Context_t *m1_context;
uint32_t mico_rtos_get_time(void){return now;}
void mico_thread_msleep(unsigned n){now+=n;}
static int persist(void *context){(void)context;writes++;if(fail_flash)return -1;flash=live;return 0;}
#undef mico_system_context_update
#define mico_system_context_update persist
PRODUCTION
void m1_alarm_get(m1_alarm_config_t *out){memset(out,0,sizeof(*out));out->reserved[0]=M1_ALARM_SCHEMA;}
void m1_wake_alarm_stop(void){stops++;m1_state.wake_alarm_active=false;}
static uint8_t sent[12],profile_status;
static unsigned sends;
static bool uart_fail;
#define MICO_UART_1 1
static int MicoUartSend(unsigned port,const void *p,unsigned n){assert(port==1&&n==12);memcpy(sent,p,n);sends++;return uart_fail?-1:0;}
#include "M1/co2_source.c"
#include "M1/display_source.c"
#include "M1/mcu.c"
static json_object *call(const char *text,bool data,const char *wanted){
    const char *code;json_object *p=json_tokener_parse(text);assert(p);
    json_object *out=data?m1_display_data_set(p,&code):m1_display_source_set(p,&code);
    assert(!strcmp(code,wanted));json_object_put(p);return out;
}
static void done(json_object *o){assert(o);json_object_put(o);}
static void reply(void){
    uint8_t p[20]={0x23,0x70,sent[2],sent[3],0};p[19]=0x21;
    if(sent[2]==0){p[5]=2;p[6]=0x3c;p[14]=1;p[15]=3;p[16]=1;p[18]=2;}
    if(sent[2]==22)p[4]=profile_status;
    if(sent[2]==23){
        unsigned slot=sent[4];const m1_display_value_t *v=&dual_sent[slot];
        p[5]=2;p[6]=dual_selected;p[7]=dual_selected&&v->valid;p[8]=slot;
        p[9]=v->number>>8;p[10]=v->number;p[11]=v->format;p[12]=v->color;p[13]=v->ttl;
    }
    assert(m1_mcu_rx(p));
}
static const char *settings="{\"revision\":0,\"selected_source\":\"user\",\"double_click\":\"switch_display\",\"lanes\":["
 "{\"slot\":0,\"source\":\"api\",\"device_id\":\"\",\"metric\":\"co2\",\"unit\":\"ppm\",\"decimals\":2,\"scale\":1000,\"ttl_seconds\":20,\"color\":{\"mode\":\"thresholds\",\"low\":1000,\"high\":1500}},"
 "{\"slot\":1,\"source\":\"api\",\"device_id\":\"\",\"metric\":\"custom\",\"unit\":\"W\",\"decimals\":1,\"scale\":1000,\"ttl_seconds\":4,\"color\":{\"mode\":\"fixed\",\"value\":\"green\"}}]}";
int main(void){
    m1_display_defaults(&live.display);live.co2_source.magic=M1_CO2_SOURCE_MAGIC;
    live.co2=(m1_co2_config_t){.magic=M1_CO2_MAGIC,.interval=5,.low=1000,.high=1500};
    assert(sizeof(live.display)==76 && sizeof(m1_display_lane_config_t)==32);
    const app_storage_t original=live;fail_flash=true;assert(!call(settings,false,"storage_error"));
    assert(!memcmp(&live,&original,sizeof(live)));fail_flash=false;
    done(call(settings,false,"accepted"));assert(live.display.revision==1&&live.display.configured&&writes==2);
    assert(!call(settings,false,"busy")&&writes==2);
    done(call("{\"revision\":1,\"slot\":0,\"value\":1234,\"unit\":\"ppm\",\"ttl_seconds\":20}",true,"accepted"));
    done(call("{\"revision\":1,\"slot\":1,\"value\":1200,\"unit\":\"W\",\"ttl_seconds\":4}",true,"accepted"));
    assert(writes==2);m1_display_value_t v;
    m1_display_source_value(&live.display,0,&v);assert(v.valid&&v.number==123&&v.format==1&&v.color==3&&v.ttl==20);
    m1_display_source_value(&live.display,1,&v);assert(v.valid&&v.number==12&&v.format==2&&v.color==2&&v.ttl==4);
    assert(!call("{\"revision\":0,\"slot\":1,\"value\":0,\"unit\":\"W\",\"ttl_seconds\":4}",true,"busy"));
    assert(!call("{\"revision\":1,\"slot\":1,\"value\":0,\"unit\":\"ppm\",\"ttl_seconds\":4}",true,"invalid_params"));
    assert(!call("{\"revision\":1,\"slot\":1,\"value\":0,\"unit\":\"W\",\"ttl_seconds\":5}",true,"invalid_params"));
    assert(!call("{\"revision\":1,\"slot\":1,\"value\":-1,\"unit\":\"W\",\"ttl_seconds\":4}",true,"invalid_params"));
    assert(!call("{\"revision\":1,\"slot\":1,\"ttl_seconds\":0,\"value\":0}",true,"invalid_params"));
    m1_state.sample=(m1_sample_t){.received=true,.hcho_ug_m3=321,.pm25_ug_m3=444,.updated_ms=now};
    m1_state.uart_ready=m1_state.mcu_version_received=true;
    m1_state.mcu_extended=m1_state.mcu_display_source=true; /* scalar schema 1 only */
    json_object *old=m1_display_source_json(),*old_runtime=json_object_object_get(old,"runtime");
    assert(!json_object_get_boolean(json_object_object_get(old_runtime,"mcu_supported")));
    assert(!strcmp(json_object_get_string(json_object_object_get(old_runtime,"confirmation")),"requires_mcu_upgrade"));json_object_put(old);
    m1_mcu_tick();assert(sent[2]==0);reply();assert(m1_state.mcu_display_schema==2);
    const unsigned ops[]={21,21,22,23,23};
    for(unsigned i=0;i<5;i++){
        now+=250;m1_mcu_tick();assert(sent[2]==ops[i]);
        if(i==0)assert(sent[9]==0&&sent[4]==0&&sent[5]==123&&sent[6]==1);
        if(i==1)assert(sent[9]==1&&sent[5]==12&&sent[6]==2);
        if(i==2)assert(sent[4]==1);
        reply();
    }
    json_object *out=m1_display_source_json(),*runtime=json_object_object_get(out,"runtime");
    assert(json_object_get_boolean(json_object_object_get(runtime,"confirmed")));
    const char *json_path=getenv("PHICOMM_TEST_DISPLAY_JSON");
    if(json_path){FILE *file=fopen(json_path,"w");assert(file);fputs(json_object_to_json_string(out),file);assert(!fclose(file));}
    json_object_put(out);
    uint8_t button[20]={0x23,0x71,1,2,1};button[19]=0x21;
    m1_state.wake_alarm_active=true;assert(m1_button_rx(button)&&stops==1&&manual_display_mode<0);
    assert(m1_button_rx(button)&&stops==1);button[4]++;
    assert(m1_button_rx(button)&&manual_display_mode==0);button[4]++;
    assert(m1_button_rx(button)&&manual_display_mode==1);
    now=5000;m1_display_source_value(&live.display,1,&v);assert(!v.valid&&v.reason==M1_DISPLAY_STALE);
    m1_display_source_value(&live.display,0,&v);assert(v.valid&&v.ttl==16);
    assert(m1_state.sample.hcho_ug_m3==321&&m1_state.sample.pm25_ug_m3==444);
    done(call("{\"revision\":1,\"slot\":0,\"ttl_seconds\":0}",true,"accepted"));
    m1_display_source_value(&live.display,0,&v);assert(!v.valid&&writes==2);
    /* Legacy writes select legacy only when the exact same flash transaction
     * commits. Both the dual configuration and profile survive failed writes. */
    m1_co2_source_config_t legacy=live.co2_source;legacy.double_click=1;
    fail_flash=true;assert(!strcmp(m1_co2_source_config_save(&legacy),"storage_error"));
    assert(live.display.configured&&live.display.revision==1&&live.co2_source.revision==0);
    fail_flash=false;assert(!strcmp(m1_co2_source_config_save(&legacy),"accepted"));
    assert(!live.display.configured&&live.display.revision==2&&live.display.lanes[1].unit==M1_DISPLAY_WATTS);
    m1_display_config_t next=live.display;next.configured=1;next.lanes[0]=(m1_display_lane_config_t){
        .source=M1_DISPLAY_LOCAL,.metric=M1_DISPLAY_CO2,.unit=M1_DISPLAY_PPM,.decimals=2,.scale=1000,.ttl_seconds=20};
    next.lanes[1]=(m1_display_lane_config_t){.source=M1_DISPLAY_MQTT,.metric=M1_DISPLAY_FAN_SPEED_PERCENT,
        .unit=M1_DISPLAY_PERCENT,.scale=1,.ttl_seconds=60};strcpy(next.lanes[1].device_id,"123456abcdef");
    assert(!strcmp(m1_display_config_save(&next),"accepted"));
    json_object *sample=json_tokener_parse("{\"boot\":42,\"sequence\":1,\"model\":\"a1\",\"sensors\":{\"fan_speed_percent\":{\"valid\":true,\"value\":60,\"unit\":\"%\",\"age_ms\":0}}}");
    m1_display_source_sample("ffffffffffff",sample);m1_display_source_value(&live.display,1,&v);assert(!v.valid);
    m1_display_source_sample("123456abcdef",sample);m1_display_source_value(&live.display,1,&v);assert(v.valid&&v.number==60);
    json_object *field=json_object_object_get(json_object_object_get(sample,"sensors"),"fan_speed_percent");
    json_object_object_add(field,"value",json_object_new_int(90));m1_display_source_sample("123456abcdef",sample);
    m1_display_source_value(&live.display,1,&v);assert(v.valid&&v.number==60);
    json_object_object_add(sample,"sequence",json_object_new_int(2));json_object_object_del(sample,"model");
    m1_display_source_sample("123456abcdef",sample);m1_display_source_value(&live.display,1,&v);assert(v.valid&&v.number==60);
    json_object_object_add(sample,"model",json_object_new_string("tc1"));
    m1_display_source_sample("123456abcdef",sample);m1_display_source_value(&live.display,1,&v);assert(v.valid&&v.number==60);
    json_object_object_add(sample,"model",json_object_new_string("a1"));json_object_object_add(sample,"sequence",json_object_new_int(1));
    m1_display_source_offline("tc1","123456abcdef");m1_display_source_value(&live.display,1,&v);assert(v.valid);
    m1_display_source_offline("a1","123456abcdef");m1_display_source_value(&live.display,1,&v);assert(!v.valid&&v.reason==M1_DISPLAY_OFFLINE);
    m1_display_source_sample("123456abcdef",sample);m1_display_source_value(&live.display,1,&v);assert(!v.valid);
    json_object_object_add(sample,"sequence",json_object_new_int(2));json_object_object_add(field,"value",json_object_new_int(101));
    m1_display_source_sample("123456abcdef",sample);m1_display_source_value(&live.display,1,&v);assert(!v.valid);
    json_object_object_add(sample,"sequence",json_object_new_int(3));json_object_object_add(field,"value",json_object_new_int(75));
    m1_display_source_sample("123456abcdef",sample);m1_display_source_value(&live.display,1,&v);assert(v.valid&&v.number==75);
    json_object_object_add(sample,"sequence",json_object_new_int(4));json_object_object_add(field,"age_ms",json_object_new_int(20000));
    m1_display_source_sample("123456abcdef",sample);m1_display_source_value(&live.display,1,&v);assert(!v.valid);
    json_object_object_add(sample,"sequence",json_object_new_int(5));json_object_object_add(field,"age_ms",json_object_new_int(19999));
    m1_display_source_sample("123456abcdef",sample);m1_display_source_value(&live.display,1,&v);assert(v.valid&&v.ttl==1);
    now++;m1_display_source_value(&live.display,1,&v);assert(!v.valid&&v.reason==M1_DISPLAY_STALE);
    json_object_object_add(sample,"boot",json_object_new_int(43));json_object_object_add(sample,"sequence",json_object_new_int(1));
    json_object_object_add(field,"age_ms",json_object_new_int(0));json_object_object_add(field,"value",json_object_new_int(50));
    m1_display_source_sample("123456abcdef",sample);m1_display_source_value(&live.display,1,&v);assert(v.valid&&v.number==50);
    json_object_object_add(sample,"boot",json_object_new_int(42));json_object_object_add(sample,"sequence",json_object_new_int(999));
    json_object_object_add(field,"value",json_object_new_int(99));
    m1_display_source_sample("123456abcdef",sample);m1_display_source_value(&live.display,1,&v);assert(v.valid&&v.number==50);
    m1_state.scd40=(scd40_state_t){.ready=true,.received=true,.co2_ppm=2000,.updated_ms=now};
    m1_display_source_value(&live.display,0,&v);assert(v.valid&&v.number==200&&v.format==1);
    m1_display_source_mqtt_reset();m1_display_source_value(&live.display,1,&v);assert(!v.valid);
    m1_display_source_value(&live.display,0,&v);assert(v.valid);json_object_put(sample);
    m1_display_lane_config_t hcho={.source=M1_DISPLAY_LOCAL,.metric=M1_DISPLAY_HCHO,.unit=M1_DISPLAY_MG_M3,.scale=1,.decimals=2};
    assert(m1_display_encode_value(&hcho,0,0.12f,2,2,&v)&&v.format==5&&v.number==12);
    hcho.scale=1000;assert(m1_display_encode_value(&hcho,0,120.0f,2,2,&v)&&v.format==1);
    hcho.scale=1;assert(!m1_display_encode_value(&hcho,0,10.0f,2,2,&v));
    assert(!m1_lock&&!config_lock);return 0;
}
'''.replace('PRODUCTION', production)
        return source

    def test_dual_configuration_leases_and_uart_readback(self):
        from tests.test_stock_runtime import run_host
        run_host(self._dual_source())

    def _run_uart(self, body, *, original=True):
        from tests.test_stock_runtime import run_host
        source = self._dual_source().split('int main(void){', 1)[0]
        if original:
            source = source.replace(r'\"selected_source\":\"user\"', r'\"selected_source\":\"original\"')
        source += r'''
static void values(void){
    char p[160];
    snprintf(p,sizeof(p),"{\"revision\":%u,\"slot\":0,\"value\":1234,\"unit\":\"ppm\",\"ttl_seconds\":20}",live.display.revision);
    done(call(p,true,"accepted"));
    snprintf(p,sizeof(p),"{\"revision\":%u,\"slot\":1,\"value\":1200,\"unit\":\"W\",\"ttl_seconds\":4}",live.display.revision);
    done(call(p,true,"accepted"));
}
static void step(unsigned op,unsigned elapsed){
    unsigned before=sends;now+=elapsed;m1_mcu_tick();assert(sends==before+1&&sent[2]==op);
}
static void begin(void){
    m1_display_defaults(&live.display);live.co2_source.magic=M1_CO2_SOURCE_MAGIC;
    live.co2=(m1_co2_config_t){.magic=M1_CO2_MAGIC,.interval=5,.low=1000,.high=1500};
    done(call(settings,false,"accepted"));values();
    m1_state.uart_ready=m1_state.mcu_version_received=true;
    m1_state.mcu_extended=m1_state.mcu_display_source=true;
    step(0,0);reply();assert(m1_state.mcu_display_schema==2);
}
static void cache(void){
    step(21,250);assert(sent[9]==0);reply();
    step(21,250);assert(sent[9]==1);reply();
}
static void query(unsigned slot,unsigned elapsed){step(23,elapsed);assert(sent[4]==slot);reply();}
static void confirmed(bool global,bool left,bool right){
    const bool expected[]={left,right};m1_display_readback_t readback[2];bool selected;
    m1_display_readback(readback,&selected);
    json_object *out=m1_display_source_json(),*runtime=json_object_object_get(out,"runtime");
    assert(json_object_get_boolean(json_object_object_get(runtime,"confirmed"))==global);
    json_object *lanes=json_object_object_get(runtime,"lanes");
    for(unsigned i=0;i<2;i++){
        json_object *lane=json_object_array_get_idx(lanes,i);
        assert(readback[i].confirmed==expected[i]);
        assert(json_object_get_boolean(json_object_object_get(lane,"confirmed"))==expected[i]);
        assert(json_object_get_boolean(json_object_object_get(lane,"effective"))==(expected[i]&&readback[i].showing));
    }
    json_object_put(out);
}
static void complete(void){cache();step(22,250);reply();query(0,250);query(1,250);confirmed(true,true,true);}
int main(void){
BODY
    assert(!m1_lock&&!config_lock);return 0;
}
'''.replace('BODY', body)
        run_host(source)

    def test_original_cached_lanes_require_positive_profile_ack(self):
        self._run_uart(r'''
    begin();cache();step(22,250);assert(!sent[4]);
    profile_status=2;reply();profile_status=0;
    query(0,2000);confirmed(false,false,false);
    query(1,250);confirmed(false,false,false);
''')

    def test_original_profile_requires_two_independent_queries(self):
        self._run_uart(r'''
    begin();cache();step(22,250);assert(!sent[4]);confirmed(false,false,false);reply();
    query(0,250);confirmed(false,true,false);
    reply();confirmed(false,true,false); /* A duplicate lane-0 reply is not lane 1. */
    query(1,250);confirmed(true,true,true);
''')

    def test_profile_timeout_and_late_ack_cannot_confirm_cached_lanes(self):
        self._run_uart(r'''
    begin();cache();step(22,250);
    uint8_t late[20]={0x23,0x70,22,sent[3],0};late[19]=0x21;
    unsigned before=sends;now+=1000;m1_mcu_tick();assert(sends==before&&!auto_waiting);
    assert(m1_mcu_rx(late));confirmed(false,false,false);
    query(0,2000);query(1,250);confirmed(false,false,false);
    values();complete();
''')

    def test_profile_uart_send_failure_cannot_confirm_cached_lanes(self):
        self._run_uart(r'''
    begin();cache();uart_fail=true;step(22,250);uart_fail=false;
    assert(!auto_waiting&&m1_state.uart_errors==1);
    query(0,2000);query(1,250);confirmed(false,false,false);
    values();complete();
''')

    def test_late_profile_ack_before_next_tick_is_not_success(self):
        self._run_uart(r'''
    begin();cache();step(22,250);now+=1000;
    reply();assert(!auto_waiting); /* RX runs before the owner's next tick. */
    query(0,2000);query(1,250);confirmed(false,false,false);
    values();complete();
''')

    def test_rejected_refresh_revokes_previous_success(self):
        self._run_uart(r'''
    begin();complete();cache();step(22,250);profile_status=2;reply();profile_status=0;
    confirmed(false,false,false);query(0,2000);query(1,250);confirmed(false,false,false);
    values();complete();
''')

    def test_ack_matches_the_actual_uart_operation_and_sequence(self):
        self._run_uart(r'''
    begin();cache();step(22,250);
    uint8_t wrong[20]={0x23,0x70,22,(uint8_t)(sent[3]+1),0};wrong[19]=0x21;
    assert(m1_mcu_rx(wrong));assert(auto_waiting);confirmed(false,false,false);
    wrong[2]=21;wrong[3]=sent[3];assert(m1_mcu_rx(wrong));assert(auto_waiting);
    reply();query(0,250);confirmed(false,true,false);query(1,250);confirmed(true,true,true);
''')

    def test_revision_change_during_profile_request_rejects_old_ack(self):
        self._run_uart(r'''
    begin();cache();step(22,250);
    m1_display_config_t next=live.display;
    assert(!strcmp(m1_display_config_save(&next),"accepted"));values();
    reply();confirmed(false,false,false);complete();
''')

    def test_manual_selection_change_invalidates_pending_and_completed_ack(self):
        self._run_uart(r'''
    begin();cache();step(22,250);assert(sent[4]==1);
    uint8_t button[20]={0x23,0x71,1,2,1};button[19]=0x21;
    assert(m1_button_rx(button)&&manual_display_mode==0);
    reply();confirmed(false,false,false);complete();
    button[4]++;assert(m1_button_rx(button)&&manual_display_mode==1);
    confirmed(false,false,false);values();complete();
''', original=False)

    def test_legacy_profile_change_requires_new_dual_ack(self):
        self._run_uart(r'''
    begin();complete();
    m1_co2_source_config_t legacy=live.co2_source;
    assert(!strcmp(m1_co2_source_config_save(&legacy),"accepted"));
    confirmed(false,false,false);step(17,250);reply();
    m1_display_config_t next=live.display;next.configured=1;
    assert(!strcmp(m1_display_config_save(&next),"accepted"));values();
    confirmed(false,false,false);complete();
''')

    def test_mcu_reboot_and_capability_probe_require_new_profile_ack(self):
        self._run_uart(r'''
    begin();complete();
    uint8_t boot[20]={0x23,0x17};boot[19]=0x21;assert(m1_mcu_rx(boot));
    confirmed(false,false,false);m1_state.mcu_version_received=true;
    step(0,0);reply();confirmed(false,false,false);values();complete();
''')

    def test_manual_profile_request_and_upgrade_revoke_confirmation(self):
        self._run_uart(r'''
    begin();complete();
    uint8_t request[12]={0x23,0x70,18,1};request[11]=0x21;uint8_t response[20];
    assert(!m1_mcu_exchange(request,sizeof(request),response));confirmed(false,false,false);
    manual_until=auto_due=now;values();complete();
    uint8_t upgrade[12]={0x23,0x12,0,164};upgrade[11]=0x21;
    assert(!m1_mcu_exchange(upgrade,sizeof(upgrade),response));
    assert(m1_state.mcu_updating);confirmed(false,false,false);
''')

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
void m1_display_config_get(m1_display_config_t *out){m1_display_defaults(out);}
void m1_display_switch_get(m1_display_switch_config_t *out){*out=(m1_display_switch_config_t){0};}
bool m1_display_lane_get(unsigned slot,m1_display_lane_config_t *out,uint32_t *revision){m1_display_config_t c;m1_display_defaults(&c);*out=c.lanes[slot];*revision=0;return false;}
void m1_alarm_get(m1_alarm_config_t *out){memset(out,0,sizeof(*out));out->reserved[0]=M1_ALARM_SCHEMA;}
const char *m1_co2_source_config_save(const m1_co2_source_config_t *c){(void)c;return "unsupported";}
static unsigned alarm_stops;
void m1_wake_alarm_stop(void){alarm_stops++;m1_state.wake_alarm_active=false;}
static uint8_t sent[12];static unsigned sends;
static int MicoUartSend(unsigned port,const void *data,unsigned n){assert(port==1&&n==12);memcpy(sent,data,n);sends++;return 0;}
#include "M1/co2_source.c"
#include "M1/display_source.c"
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

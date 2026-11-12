"""Execute real MP display parser, descriptor leases and complete snapshots."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class DisplayMPTest(unittest.TestCase):
 def test_typed_commands_and_sensor_recovery(self):
  source=r'''
#include <assert.h>
#include "M1/m1.h"
static uint32_t now=1000;mico_mutex_t m1_lock;m1_state_t m1_state;
static m1_display_config_t live;static m1_co2_source_config_t co2;static unsigned saves;
uint32_t mico_rtos_get_time(void){return now;}
void m1_display_config_get(m1_display_config_t *out){*out=live;}
bool m1_display_lane_get(unsigned i,m1_display_lane_config_t *l,m1_display_remote_t *r,uint32_t *rev){if(i>=2)return false;*l=live.lanes[i];if(r)*r=live.remote[i];*rev=live.revision;return true;}
const char *m1_display_config_save(const m1_display_config_t *c){if(c->revision!=live.revision)return "busy";live=*c;live.revision++;saves++;return "accepted";}
void m1_co2_source_config_get(m1_co2_source_config_t *out){*out=co2;}
const char *m1_co2_source_config_save(const m1_co2_source_config_t *c){co2=*c;co2.revision++;saves++;return "accepted";}
const char *m1_display_data_save(unsigned slot,uint32_t revision,float value,unsigned unit,unsigned ttl,int color){if(revision!=live.revision)return "busy";if(live.lanes[slot].source!=M1_DISPLAY_API||unit!=live.lanes[slot].unit)return "invalid_params";m1_display_data_store(slot,revision,value,ttl,color);return "accepted";}
const char *m1_display_frame_save(unsigned slot,uint32_t revision,unsigned number,unsigned format,unsigned lamps,unsigned ttl){if(revision!=live.revision)return "busy";if(live.lanes[slot].source!=M1_DISPLAY_API)return "invalid_params";m1_display_frame_store(slot,revision,number,format,lamps,ttl);return "accepted";}
void m1_display_readback(m1_display_readback_t out[2],bool *selected){memset(out,0,2*sizeof(*out));*selected=false;}
#include "M1/co2_source.c"
#include "M1/display_source.c"
static ph_buf *finish(ph_writer *w,ph_value *v){ph_buf *b=phw_take(w);assert(b);assert(phv_parse(PH_WIRE_MSGPACK,b->buf,b->len,v));return b;}
static void metadata(uint32_t boot,uint32_t rev,const char *unit){ph_writer w;ph_value v;assert(phw_init(&w,PH_WIRE_MSGPACK));ph_sensor_metadata_begin(&w,"123456abcdef",boot,rev);ph_sensor_descriptor_write(&w,"fan_rpm","Fan speed",unit);ph_sensor_finish(&w);ph_buf *b=finish(&w,&v);m1_display_source_metadata("123456abcdef",&v);memset(b->buf,0,b->len);ph_buf_free(b);}
static void sample(uint32_t boot,uint32_t seq,uint32_t rev,bool present,bool valid,double value,uint32_t age){ph_writer w;ph_value v;assert(phw_init(&w,PH_WIRE_MSGPACK));ph_sensor_snapshot_begin(&w,"123456abcdef",boot,seq,rev);if(present)ph_sensor_reading_write(&w,"fan_rpm",value,valid,age);ph_sensor_finish(&w);ph_buf *b=finish(&w,&v);m1_display_source_sample("123456abcdef",&v);memset(b->buf,0,b->len);ph_buf_free(b);}
static m1_display_value_t readvalue(void){m1_display_value_t v;m1_display_source_value(&live,0,&v);return v;}
static void raw(unsigned slot,unsigned number,unsigned format,unsigned mask){ph_writer w;ph_value v;assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,NULL,false);phw_u32(&w,"revision",live.revision);phw_u32(&w,"slot",slot);phw_u32(&w,"number",number);phw_bool(&w,"decimal",format&1);phw_bool(&w,"show_label",format&2);phw_bool(&w,"show_unit",format&4);phw_u32(&w,"ttl_seconds",20);display_lamps_write(&w,"lamps",mask);phw_end(&w);ph_buf *b=finish(&w,&v);const char *code;ph_writer out;assert(phw_init(&out,PH_WIRE_MSGPACK));assert(m1_display_mp("display_value.set",&v,&out,&code));assert(!strcmp(code,"accepted"));ph_buf *response=finish(&out,&v);ph_buf_free(response);ph_buf_free(b);}
int main(void){
 m1_display_defaults(&live);live.configured=1;live.reserved=M1_DISPLAY_FORMATTED;live.revision=1;
 live.lanes[0]=(m1_display_lane_config_t){.source=M1_DISPLAY_MQTT,.metric=M1_DISPLAY_CUSTOM,.unit=M1_DISPLAY_UNITLESS,.ttl_seconds=20,.scale=1};strcpy(live.lanes[0].device_id,"123456abcdef");strcpy(live.remote[0].field_id,"fan_rpm");strcpy(live.remote[0].unit,"rpm");live.style[0].mode=M1_DISPLAY_INTEGER;
 sample(1,1,1,true,true,500,0);assert(!readvalue().valid);metadata(1,1,"rpm");sample(1,2,1,true,true,500,0);assert(readvalue().valid&&readvalue().number==500);
 sample(1,2,1,true,true,900,0);assert(readvalue().number==500);now+=19999;assert(readvalue().valid);now++;assert(!readvalue().valid);
 now++;sample(1,3,1,true,true,600,0);assert(readvalue().number==600);m1_display_source_offline("newmodel","123456abcdef");assert(!readvalue().valid);sample(1,3,1,true,true,600,0);assert(!readvalue().valid);
 sample(1,4,1,false,false,0,0);assert(!readvalue().valid);sample(1,5,1,true,true,700,0);assert(readvalue().valid);
 metadata(1,2,"badunit");assert(!readvalue().valid);sample(1,6,2,true,true,700,0);assert(!readvalue().valid);
 metadata(1,3,"rpm");sample(1,7,3,true,true,800,19999);assert(readvalue().valid&&readvalue().ttl==1);now++;assert(!readvalue().valid);
 metadata(2,1,"rpm");sample(2,1,1,true,true,321,0);assert(readvalue().valid);metadata(1,99,"rpm");sample(1,999,99,true,true,999,0);assert(readvalue().number==321);
 metadata(2,2,"rpm");sample(2,2,2,true,false,0,0);assert(!readvalue().valid);sample(2,3,2,true,true,0,0);assert(readvalue().valid&&readvalue().number==0);
 live.lanes[0].source=live.lanes[1].source=M1_DISPLAY_API;live.remote[0].unit[0]=0;m1_display_source_reset();
 for(unsigned mask=0;mask<512;mask++){raw(0,999,1,mask);m1_display_value_t v=readvalue();assert(v.valid&&v.number==999&&v.format==1&&v.lamps==mask);}
 raw(1,999,6,511);m1_display_value_t second;m1_display_source_value(&live,1,&second);assert(second.valid&&second.number==999&&second.format==6&&second.lamps==511);
 assert(!saves&&!m1_lock);return 0;
}
'''
  with tempfile.TemporaryDirectory() as d:
   src=Path(d)/'test.c';exe=Path(d)/'test';src.write_text(source)
   json_dir=Path(os.environ.get('PHICOMM_TEST_JSON_DIR',ROOT/'build/m1-sdk/mico-os/libraries/utilities/json_c'))
   # M1 headers still contain legacy service declarations during staged migration;
   # this test links no JSON implementation and no JSON symbol is used.
   subprocess.run([os.environ.get('CC','cc'),'-std=gnu99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(ROOT/'tests/host'),'-I',str(ROOT),'-I',str(json_dir),str(src),str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c'),'-lm','-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True)
if __name__=='__main__':unittest.main()

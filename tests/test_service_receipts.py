"""Frozen reply ownership, bounded retention and cache invalidation regressions."""
from pathlib import Path
import unittest

from tests import test_service_mp

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = (ROOT / 'tests/service_mp/fixture.inc').read_text().replace(
    'bool service_product_mp(', 'bool fixture_product_mp(')
FIXTURE += r'''
static unsigned snapshot_bytes=1024,snapshot_seed;
static bool unrelated_snapshot;
static ph_buf *snapshot_request(unsigned number){
 char id[32];snprintf(id,sizeof(id),"snapshot-%04u",number);return request("snapshot.set",id,-1);
}
bool service_product_mp(const char *action,const ph_value *p,ph_writer *w,const char **out){
 if(strcmp(action,"snapshot.set"))return fixture_product_mp(action,p,w,out);
 if(!phv_keys(p,"")){*out="invalid_params";object(w);return true;}
 unsigned char data[4096];assert(snapshot_bytes<=sizeof(data));
 uint32_t random=snapshot_seed+1;
 for(unsigned i=0;i<snapshot_bytes;i++){
  random=random*1664525u+1013904223u;
  data[i]=unrelated_snapshot?(unsigned char)(random>>24):(unsigned char)(i*31u);
 }
 mutations++;*out="accepted";phw_begin(w,NULL,false);
 phw_u32(w,"sample",snapshot_seed);phw_bytes_value(w,"payload",data,snapshot_bytes);phw_end(w);return true;
}
'''


class ReceiptSnapshots(unittest.TestCase):
    def run_snapshot(self, body, **kwargs):
        test_service_mp.NativeService.run_c(self, body, fixture=FIXTURE, **kwargs)

    def test_replay_exact_bytes_owned_outputs_and_eight_slot_turnover(self):
        self.run_snapshot(r'''
int main(void){
 service_init();unsigned char expected[8][2048];size_t sizes[8]={0};
 ph_buf *held=NULL;
 for(unsigned i=0;i<64;i++){
  snapshot_seed=i;ph_buf *q=snapshot_request(i),*out=service_command_mp(q->buf,q->len);code(out,"accepted");
  assert(out->len<sizeof(expected[0]));sizes[i%8]=out->len;memcpy(expected[i%8],out->buf,out->len);
  if(i==0)held=out;else{memset(out->buf,0,out->len);ph_buf_free(out);}
  memset(q->buf,0,q->len);ph_buf_free(q);
  unsigned calls=mutations;
  /* Every retained request must replay its original bytes after later values,
   * base eviction and promotion. Returned bytes are freely writable. */
  for(unsigned j=i<7?0:i-7;j<=i;j++){
   q=snapshot_request(j);out=service_command_mp(q->buf,q->len);code(out,"accepted");
   assert(out->len==sizes[j%8]&&!memcmp(out->buf,expected[j%8],out->len));
   memset(out->buf,0,out->len);ph_buf_free(out);ph_buf_free(q);
  }
  assert(mutations==calls);
  if(i>=8)code(held,"accepted");
  assert(total_allocations-total_frees==live_blocks);
 }
 code(held,"accepted");ph_buf_free(held);
 ph_buf *q=request("snapshot.set","snapshot-0063",1),*out=service_command_mp(q->buf,q->len);
 code(out,"conflict");ph_buf_free(out);ph_buf_free(q);assert(mutations==64);
 command_code(snapshot_request(0),"accepted");assert(mutations==65);
 /* A complete replacement with small, unrelated receipts releases all bases
  * and differences; each slot keeps one allocation, below the old cost. */
 size_t old_budget=0;snapshot_bytes=10;
 for(unsigned i=100;i<108;i++){
  q=snapshot_request(i);out=service_command_mp(q->buf,q->len);old_budget+=sizeof(ph_buf)+out->len+1;
  ph_buf_free(out);ph_buf_free(q);
 }
 assert(live_blocks==8&&live_bytes<=old_budget);return 0;
}
''')

    def test_small_unrelated_and_orphaned_bases_never_exceed_old_storage(self):
        self.run_snapshot(r'''
int main(void){
 service_init();size_t old_sizes[8]={0};
 for(unsigned i=0;i<192;i++){
  /* Eight similar replies first, then displace the base and its dependents
   * one at a time with unrelated and differently sized data. */
  snapshot_seed=i;unrelated_snapshot=i>=8;
  snapshot_bytes=i<16?1024:i<80?17:i<128?1024:512+(i%5)*11;
  ph_buf *q=snapshot_request(i),*out=service_command_mp(q->buf,q->len);code(out,"accepted");
  old_sizes[i%8]=sizeof(ph_buf)+out->len+1;ph_buf_free(out);ph_buf_free(q);
  size_t old_budget=0;for(unsigned j=0;j<8;j++)old_budget+=old_sizes[j];
  assert(live_bytes<=old_budget);
  assert(total_allocations-total_frees==live_blocks);
 }
 assert(live_blocks==8);return 0;
}
''')

    def test_allocation_failures_preserve_tombstones_and_other_receipts(self):
        self.run_snapshot(r'''
int main(int argc,char **argv){
 (void)argc;service_init();unsigned char expected[8][2048];size_t sizes[8];
 for(unsigned i=0;i<8;i++){
  snapshot_seed=i;ph_buf *q=snapshot_request(i),*out=service_command_mp(q->buf,q->len);code(out,"accepted");
  sizes[i]=out->len;memcpy(expected[i],out->buf,out->len);ph_buf_free(out);ph_buf_free(q);
 }
 ph_buf *q=snapshot_request(8);snapshot_seed=8;
 allocation_budget=strtol(argv[1],NULL,10);ph_buf *out=service_command_mp(q->buf,q->len);
 allocation_budget=-1;ph_buf_free(out);unsigned before=mutations;
 out=service_command_mp(q->buf,q->len);assert(out&&mutations==9);ph_buf_free(out);
 out=service_command_mp(q->buf,q->len);assert(out&&mutations==9);ph_buf_free(out);ph_buf_free(q);
 assert(before==8||before==9);
 for(unsigned i=1;i<8;i++){
  q=snapshot_request(i);out=service_command_mp(q->buf,q->len);code(out,"accepted");
  assert(out->len==sizes[i]&&!memcmp(out->buf,expected[i],out->len));ph_buf_free(out);ph_buf_free(q);
 }
 /* Replay allocations can fail without consuming or replacing the receipt. */
 q=snapshot_request(7);allocation_budget=0;assert(!service_command_mp(q->buf,q->len));allocation_budget=-1;
 out=service_command_mp(q->buf,q->len);code(out,"accepted");assert(mutations==9);ph_buf_free(out);ph_buf_free(q);
 command_code(request("snapshot.set","snapshot-0008",1),"conflict");assert(mutations==9);
 assert(total_allocations-total_frees==live_blocks);return 0;
}
''', runs=16)

    def test_capabilities_follow_authoritative_changes_and_status_follows_commands(self):
        self.run_snapshot(r'''
static void read_caps(unsigned expected){ph_buf *out=service_query_mp("capabilities.get");code(out,"ok");ph_buf_free(out);assert(caps==expected);}
static void read_status(unsigned expected){ph_buf *out=service_query_mp("status.get");code(out,"ok");ph_buf_free(out);assert(captures==expected);}
int main(void){
 service_init();read_caps(1);read_status(1);
 command_code(snapshot_request(0),"accepted");read_caps(1);read_status(2);
 command_code(snapshot_request(0),"accepted");read_caps(1);read_status(2);assert(mutations==1);
 command_code(request("snapshot.set","rejected",1),"invalid_params");read_caps(1);read_status(3);assert(mutations==1);
 command_code(request("missing.set","unsupported",-1),"unsupported");read_caps(1);read_status(4);
 command_code(request("system.reset","reset",1),"accepted");read_caps(1);read_status(5);
 device_config_recovery_set(true);read_caps(2);read_caps(2);
 command_code(snapshot_request(1),"migration_required");assert(mutations==2);read_caps(2);
 device_config_recovery_set(false);read_caps(3);
 service_cache_changed(SERVICE_CACHE_STATUS);read_caps(3);read_status(6);
 service_cache_changed(SERVICE_CACHE_CAPABILITIES);read_caps(4);read_status(6);
 service_cache_changed(SERVICE_CACHE_ALL);read_caps(5);read_status(7);
 return 0;
}
''')

    def test_real_m1_display_write_replay_ttl_and_local_sensor_expiry(self):
        fixture = (ROOT / 'tests/service_mp/fixture.inc').read_text().replace(
            'bool service_product_mp(', 'bool fixture_product_mp(').replace(
            'void service_product_status_mp(', 'void fixture_product_status_mp(')
        main = (ROOT / 'M1/main.c').read_text()
        owners = main[main.index('void m1_display_config_get('):
                      main.index('const char *m1_co2_source_config_save(')]
        fixture += r'''
typedef int OSStatus;
#include "M1/storage.h"
static app_storage_t display_storage;
static m1_config_t *app_config=&display_storage.base;
static mico_mutex_t config_lock=PTHREAD_MUTEX_INITIALIZER;
mico_mutex_t m1_lock=PTHREAD_MUTEX_INITIALIZER;
m1_state_t m1_state;
char m1_id[13]="aabbccddeeff";
static bool config_recovery,fail_flash;
static unsigned flash_writes;
mico_Context_t *m1_context;
static int display_persist(void *context){(void)context;flash_writes++;return fail_flash?-1:0;}
#define mico_system_context_update display_persist
''' + owners + r'''
#include "M1/display_source.c"
void m1_display_readback(m1_display_readback_t out[2],bool *selected){memset(out,0,2*sizeof(*out));*selected=false;}
void service_product_status_mp(ph_writer *w){captures++;m1_display_profile_write(w);}
bool service_product_mp(const char *action,const ph_value *p,ph_writer *w,const char **out){
 return m1_display_mp(action,p,w,out)||fixture_product_mp(action,p,w,out);
}
static ph_buf *display_request(const char *id,unsigned number){
 ph_writer w;assert(phw_init(&w,1));phw_begin(&w,NULL,false);phw_text(&w,"action","display_value.set");
 phw_text(&w,"request_id",id);phw_begin(&w,"params",false);phw_u32(&w,"revision",2);phw_u32(&w,"slot",0);
 phw_u32(&w,"number",number);phw_bool(&w,"decimal",false);phw_bool(&w,"show_label",false);phw_bool(&w,"show_unit",false);
 phw_u32(&w,"ttl_seconds",30);display_lamps_write(&w,"lamps",8);phw_end(&w);phw_end(&w);return phw_take(&w);
}
static bool reply_valid(ph_buf *out,unsigned wanted){
 ph_value data=data_value(out),runtime,lanes,lane,valid,value;bool yes;double n;
 assert(phv_get(&data,"runtime",&runtime)&&phv_get(&runtime,"lanes",&lanes)&&phv_at(&lanes,0,&lane));
 assert(phv_get(&lane,"valid",&valid)&&phv_bool(&valid,&yes));
 if(yes)assert(phv_get(&lane,"value",&value)&&phv_number(&value,&n)&&n==wanted);
 return yes;
}
'''
        test_service_mp.NativeService.run_c(self, r'''
int main(void){
 service_init();m1_display_config_t *c=&display_storage.display;m1_display_defaults(c);
 c->configured=c->selected=1;c->revision=2;c->reserved=M1_DISPLAY_FORMATTED;
 c->lanes[0]=(m1_display_lane_config_t){.source=M1_DISPLAY_API,.metric=M1_DISPLAY_CUSTOM,.unit=M1_DISPLAY_UNITLESS,.scale=1,.ttl_seconds=30};
 c->lanes[1].source=M1_DISPLAY_NONE;c->style[0].mode=M1_DISPLAY_DECIMAL_2;
 ph_buf *out=service_query_mp("capabilities.get");code(out,"ok");ph_buf_free(out);
 ph_buf *q=display_request("display-once",214),*first=service_command_mp(q->buf,q->len);code(first,"accepted");assert(reply_valid(first,214));
 out=service_query_mp("status.get");code(out,"ok");assert(reply_valid(out,214));ph_buf_free(out);
 out=service_query_mp("capabilities.get");ph_buf_free(out);assert(caps==1&&!flash_writes);
 test_clock_ms+=29000;out=service_command_mp(q->buf,q->len);code(out,"accepted");
 assert(out->len==first->len&&!memcmp(out->buf,first->buf,out->len));ph_buf_free(out);
 m1_display_value_t current;m1_display_source_value(c,0,&current);assert(current.valid&&current.ttl==1);
 test_clock_ms+=1000;m1_display_source_value(c,0,&current);assert(!current.valid&&current.reason==M1_DISPLAY_STALE);
 out=service_query_mp("display_profile.get");code(out,"ok");assert(!reply_valid(out,0));ph_buf_free(out);
 out=service_query_mp("status.get");code(out,"ok");assert(!reply_valid(out,0));ph_buf_free(out);
 out=service_command_mp(q->buf,q->len);assert(out->len==first->len&&!memcmp(out->buf,first->buf,out->len));ph_buf_free(out);
 ph_buf_free(q);ph_buf_free(first);
 command_code(display_request("display-once",215),"conflict");m1_display_source_value(c,0,&current);assert(!current.valid);
 command_code(display_request("display-next",215),"accepted");
 out=service_query_mp("status.get");code(out,"ok");assert(reply_valid(out,215));ph_buf_free(out);
 out=service_query_mp("capabilities.get");ph_buf_free(out);assert(caps==1&&!flash_writes);
 /* The real configuration owner rolls back a failed save; transient values
  * remain valid and the next full read still sees the committed profile. */
 m1_display_config_t next=*c;next.selected=0;fail_flash=true;
 assert(!strcmp(m1_display_config_save(&next),"storage_error")&&c->revision==2&&c->selected);
 m1_display_source_value(c,0,&current);assert(current.valid&&current.number==215);assert(flash_writes==1);
 /* Real physical sensor age also expires; replay never feeds a sensor sample. */
 m1_display_source_reset();c->lanes[0].source=M1_DISPLAY_LOCAL;c->lanes[0].metric=M1_DISPLAY_PM25;c->lanes[0].unit=M1_DISPLAY_UG_M3;
 c->style[0].mode=M1_DISPLAY_INTEGER;m1_state.sample=(m1_sample_t){.received=true,.pm25_status=1,.pm25_ug_m3=42,.updated_ms=test_clock_ms};
 m1_display_source_value(c,0,&current);assert(current.valid&&current.value==42);
 test_clock_ms+=M1_STALE_MS;m1_display_source_value(c,0,&current);assert(!current.valid&&current.reason==M1_DISPLAY_STALE);
 assert(m1_state.sample.pm25_ug_m3==42);return 0;
}
''', fixture=fixture, fixture_headers={'mico_system.h': '#pragma once\ntypedef struct { int unused; } mico_Context_t;\n'})


if __name__ == '__main__':
    unittest.main()

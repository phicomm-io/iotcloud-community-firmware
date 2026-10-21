#include "core.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef struct { unsigned locked, allocations, frees, fail_at; bool owner, low; uint32_t now; } fixture;
static void lock(void *p){fixture *f=p;assert(!f->locked++);}
static void unlock(void *p){fixture *f=p;assert(f->locked--==1);}
static bool owner(void *p){return ((fixture*)p)->owner;}
static uint32_t now(void *p){return ((fixture*)p)->now;}
static bool permit(void *p,size_t n){assert(n<=PH_MQTT_QUEUE_BYTES);return !((fixture*)p)->low;}
static void *allocate(void *p,size_t n){fixture*f=p;f->allocations++;return f->fail_at==f->allocations?NULL:malloc(n);}
static void release(void*p,void*q){((fixture*)p)->frees++;free(q);}
static void framing(void){
 ph_mqtt_rx r;unsigned char ok[]={0x30,3,0,1,'a',0xd0,0};
 for(size_t split=0;split<=sizeof(ok);split++){
  ph_mqtt_rx_init(&r,8);assert(ph_mqtt_rx_feed(&r,ok,split));assert(ph_mqtt_rx_feed(&r,ok+split,sizeof(ok)-split));assert(!r.state&&r.serial==2);
 }
 unsigned char huge[]={0x30,0x80,1};ph_mqtt_rx_init(&r,128);
 assert(!ph_mqtt_rx_feed(&r,huge,sizeof(huge)));assert(!ph_mqtt_rx_feed(&r,ok,sizeof(ok)));
 const unsigned char bad[][6]={{0x30,0x80,0},{0x30,0x81,0x80,0x80,0x80},{0x00,0},{0xf0,0}};
 for(unsigned i=0;i<4;i++){ph_mqtt_rx_init(&r,9000);assert(!ph_mqtt_rx_feed(&r,bad[i],i==1?5:3));}
 unsigned char terminal[]={0x30,0x7e};ph_mqtt_rx_init(&r,128);assert(ph_mqtt_rx_feed(&r,terminal,2));assert(r.left==126);
 terminal[1]=0x7f;ph_mqtt_rx_init(&r,128);assert(!ph_mqtt_rx_feed(&r,terminal,2));
}
int main(void){
 fixture f={.owner=true};ph_mqtt_hooks h={&f,lock,unlock,owner,now,permit,allocate,release};ph_mqtt_core s;
 assert(ph_mqtt_core_init(&s,&h));
 for(unsigned round=0;round<1000;round++){
  assert(ph_mqtt_protocol_begin(&s));assert(!ph_mqtt_protocol_begin(&s));assert(!ph_mqtt_dispatch_begin(&s));
  for(unsigned i=0;i<4;i++)assert(ph_mqtt_enqueue(&s,0,"command",7,"abc",3,1,false));
  unsigned n=f.allocations;assert(!ph_mqtt_enqueue(&s,0,"command",7,"abc",3,1,false)&&f.allocations==n);
  assert(!ph_mqtt_pop(&s,0));ph_mqtt_protocol_end(&s);
  ph_mqtt_item*i=ph_mqtt_pop(&s,0);assert(i&&s.count[0]==4&&s.bytes);assert(!ph_mqtt_pop(&s,0));
  assert(ph_mqtt_credit_acquire(&s,8192));assert(!ph_mqtt_credit_acquire(&s,1));
  assert(ph_mqtt_dispatch_begin(&s));assert(!ph_mqtt_dispatch_begin(&s));assert(!ph_mqtt_clear(&s));
  assert(!strcmp(ph_mqtt_item_topic(i),"command")&&!memcmp(ph_mqtt_item_payload(i),"abc",3));
  ph_mqtt_dispatch_end(&s);ph_mqtt_item_release(&s,i);ph_mqtt_item_release(&s,i);
  assert(ph_mqtt_clear(&s)&&!s.bytes&&!s.credit&&!s.count[0]);
 }
 f.owner=false;assert(!ph_mqtt_protocol_begin(&s)&&!ph_mqtt_pop(&s,0)&&!ph_mqtt_clear(&s));assert(s.owner_faults==3);f.owner=true;
 f.low=true;unsigned n=f.allocations;assert(!ph_mqtt_enqueue(&s,1,"x",1,"y",1,1,true)&&f.allocations==n);f.low=false;
 f.fail_at=f.allocations+1;assert(!ph_mqtt_enqueue(&s,1,"x",1,"y",1,1,true));assert(!s.bytes);f.fail_at=0;
 char large[8192]={0};assert(ph_mqtt_enqueue(&s,0,"x",1,large,sizeof(large),1,false));n=f.allocations;
 assert(!ph_mqtt_enqueue(&s,0,"x",1,large,sizeof(large),1,false)&&f.allocations==n);
 assert(!ph_mqtt_credit_acquire(&s,8192));assert(ph_mqtt_clear(&s));
 for(unsigned i=0;i<6;i++)assert(ph_mqtt_enqueue(&s,1,"t",1,"v",1,1,false));
 assert(!ph_mqtt_enqueue(&s,1,"t",1,"v",1,1,false));assert(ph_mqtt_clear(&s));
 assert(ph_mqtt_enqueue(&s,1,"latest",6,"old",3,1,true));
 assert(ph_mqtt_enqueue(&s,1,"latest",6,"new",3,1,true));
 assert(s.count[1]==1);ph_mqtt_item *latest=ph_mqtt_pop(&s,1);
 assert(!memcmp(ph_mqtt_item_payload(latest),"new",3));
 ph_mqtt_diagnostics d;ph_mqtt_core_snapshot(&s,&d);
 assert(d.inflight[1]==1 && d.queued[1]==0 && d.bytes && !d.credit);
 ph_mqtt_core_accepting(&s,false);n=f.allocations;
 assert(!ph_mqtt_enqueue(&s,1,"x",1,"v",1,1,false) && n==f.allocations);
 ph_mqtt_item_release(&s,latest);assert(ph_mqtt_clear(&s));
 assert(f.allocations==f.frees+1); /* one injected failed allocation */
 s.errors=s.reconnects=UINT32_MAX-1;
 ph_mqtt_record_error(&s,4,-77);ph_mqtt_record_error(&s,5,-88);
 ph_mqtt_record_reconnect(&s);ph_mqtt_record_reconnect(&s);
 ph_mqtt_core_snapshot(&s,&d);assert(d.errors==UINT32_MAX&&d.reconnects==UINT32_MAX&&d.last_stage==5&&d.last_error==-88);
 framing();puts("actual common MQTT core: bounds, owner/depth, in-flight/credit cleanup and fragmented RX framing PASS");
}

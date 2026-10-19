/* Real dispatcher + guarded SDK JSON; no physical actuator operations. */
#include "mico.h"
#define MOC 1
typedef struct {int num_of_chunks,total_memory,allocted_memory,free_memory;} micoMemInfo_t;
static micoMemInfo_t heap;
#define MicoGetMemoryInfo() (&heap)
#include "../common/service.c"
uint32_t test_now;
static bool exhaust_during_build;
static unsigned mutations;
json_object *automation_command(const char *a,json_object *p,const char **c,bool *h)
{(void)a;(void)p;(void)c;*h=false;return NULL;}
json_object *service_product_capabilities(void){return json_object_new_object();}
json_object *service_product(const char *a,json_object *p,const char **c){
 (void)p;if(!strcmp(a,"fan.set"))mutations++;
 *c=!strcmp(a,"fan.set")?"accepted":"ok";
 json_object *o=json_object_new_object();
 json_build_object_add(o,"first",json_object_new_string("complete"));
 if(exhaust_during_build)heap.free_memory=SERVICE_MEMORY_RESERVE;
 json_build_object_add(o,"second",json_object_new_string("required"));
 return o;
}
static void empty(void){json_heap_stats_t s;json_heap_snapshot(&s);unsigned retained=0;for(unsigned i=0;i<8;i++)retained+=(receipts[i].request!=NULL)+(receipts[i].response!=NULL);assert(s.live_blocks==retained);service_memory_state_t m;service_memory_snapshot(&m);assert(!m.active && !m.reserved);}
int main(void){
 heap.free_memory=64000;service_init();assert(ready);
 service_memory_mqtt(7,false,900);service_memory_state_t diagnostics;
 service_memory_snapshot(&diagnostics);assert(diagnostics.mqtt_pending==7 && diagnostics.mqtt_peak==7 && diagnostics.mqtt_max_delay==900);
 service_memory_mqtt(0,true,50);service_memory_snapshot(&diagnostics);
 assert(!diagnostics.mqtt_pending && diagnostics.mqtt_peak==7 && diagnostics.mqtt_overflow==1 && diagnostics.mqtt_max_delay==900);
 for(unsigned i=0;i<10000;i++){json_object *r=service_query("status.get");assert(r);json_object_put(r);empty();}
 uint64_t before=service_memory_failures();exhaust_during_build=true;
 assert(!service_query("status.get"));assert(service_memory_failures()>before);empty();
 heap.free_memory=SERVICE_MEMORY_RESERVE;before=service_memory_failures();
 assert(!service_query("status.get"));assert(service_memory_failures()>before);empty();
 /* Partial JSON must not turn into an apparently successful response. A
  * post-mutation failure still keeps the at-most-once receipt. */
 heap.free_memory=64000;
 const char *text="{\"action\":\"fan.set\",\"request_id\":\"resource-1\",\"params\":{}}";
 json_object *q=service_parse(text,strlen(text));assert(q);
 assert(!service_command(q));json_object_put(q);assert(mutations==1);empty();
 heap.free_memory=64000;exhaust_during_build=false;
 q=service_parse(text,strlen(text));assert(q);json_object *r=service_command(q);assert(r);
 assert(!strcmp(json_object_get_string(json_object_object_get(r,"code")),"busy"));
 json_object_put(q);json_object_put(r);assert(mutations==1);empty();
 /* Live free bytes, not an active count, determine admission. */
 unsigned claims[64];
 for(unsigned i=0;i<64;i++){claims[i]=service_memory_claim(0);assert(claims[i]);}
 for(unsigned i=0;i<64;i++)service_memory_release(claims[i]);empty();
 heap.free_memory=SERVICE_MEMORY_RESERVE+511;assert(!service_memory_claim(0));
 heap.free_memory=64000;for(unsigned i=0;i<8;i++){json_heap_free(receipts[i].request);json_heap_free(receipts[i].response);memset(&receipts[i],0,sizeof(receipts[i]));}
 empty();puts("Dispatcher: 10000 cleanup cycles, allocation pressure returns NULL/busy, incomplete response suppressed, at-most-once retained, byte-based admission 64 requests: OK");
}

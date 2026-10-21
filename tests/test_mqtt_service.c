/* Owned-wire regressions execute byte-for-byte helpers from the compiled
 * shared service. Queue/dispatch/targets are covered with actual Paho in
 * tests.test_shared_mqtt; the removed linked-reply backlog is not retained. */
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "json.h"
#include "json_heap.h"
#include "../common/json_build.h"
#include "../common/json_owned.h"
static void *watched[2];
static unsigned freed[2],wire_calls;
static bool fail_realloc;
int json_heap_permit(size_t bytes){(void)bytes;return 1;}
void __real_free(void *);
void *__real_realloc(void *,size_t);
void __wrap_free(void *pointer){
 for(unsigned i=0;i<2;i++)if(pointer && pointer==watched[i])freed[i]++;
 __real_free(pointer);
}
void *__wrap_realloc(void *pointer,size_t size){
 if(fail_realloc){fail_realloc=false;return NULL;}
 return __real_realloc(pointer,size);
}
#define MQTT_SUCCESS 0
#define MQTT_FAILURE -1
#include "mqtt_owned_helpers.inc"
static json_object *owned_reply(void){
 json_object *o=json_object_new_object(),*child=json_object_new_string("independent wire bytes");
 assert(o && child);json_build_object_add(o,"value",child);
 watched[0]=o;watched[1]=child;freed[0]=freed[1]=0;return o;
}
static int send_owned(const char *topic,const char *body,bool retained){
 assert(!strcmp(topic,"topic") && retained);wire_calls++;
 assert(freed[0]==1 && freed[1]==1);
 watched[0]=watched[1]=NULL;
 json_object *copy=json_tokener_parse(body);assert(copy);
 assert(!strcmp(json_object_get_string(json_object_object_get(copy,"value")),"independent wire bytes"));
 json_object_put(copy);return MQTT_FAILURE; /* The wire is freed on send failure too. */
}
static void test_owned_wire(void){
 assert(service_mqtt_send(send_owned,"topic",owned_reply(),true)==MQTT_FAILURE);
 assert(wire_calls==1);
 json_object *o=owned_reply();
 json_build_object_add(o,"required",NULL);
 assert(service_mqtt_send(send_owned,"topic",o,true)==MQTT_FAILURE);
 assert(wire_calls==1 && freed[0]==1 && freed[1]==1);
 watched[0]=watched[1]=NULL;
 o=owned_reply();fail_realloc=true;
 assert(service_mqtt_send(send_owned,"topic",o,true)==MQTT_FAILURE);
 assert(!fail_realloc && wire_calls==1 && freed[0]==1 && freed[1]==1);
 watched[0]=watched[1]=NULL;
}
int main(void){
 test_owned_wire();json_heap_stats_t heap;json_heap_snapshot(&heap);assert(!heap.live_blocks);
 puts("Shared MQTT actual wire helpers: graph-before-send, failed graph, serialization OOM/send failure cleanup PASS");
}

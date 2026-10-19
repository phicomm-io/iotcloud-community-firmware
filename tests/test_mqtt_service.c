#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "json.h"
#include "../common/mqtt_account.h"
static void *watched[2];
static unsigned freed[2], wire_calls;
static bool fail_realloc;
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
#include "../common/mqtt_service.inc"
static unsigned calls, executions;
static bool nested,switch_scope;
static char expected_topics[4][MQTT_TOPIC_MAX];
static unsigned topic_index;
static mqtt_account_t scope;
json_object *service_parse(const void *p,size_t n){(void)p;(void)n;return json_object_new_object();}
json_object *service_command(json_object *q){(void)q;executions++;
 if(switch_scope){switch_scope=false;assert(mqtt_account_init(&scope,1,"m1","changed","aabbccddeeff"));}
 return json_object_new_object();}
json_object *service_query(const char *a){(void)a;json_object *r=json_object_new_object();json_object_object_add(r,"data",json_object_new_object());return r;}
json_object *service_product_capabilities(void){return json_object_new_object();}
static int send_reply(const char *topic,const char *body,bool retained){
 assert(strstr(topic,"/result"));assert(body);assert(!retained);calls++;
 if(nested){nested=false;assert(service_mqtt_input(send_reply,&scope,"{}",2,false)==0);assert(calls==1);}
 return 0;
}
static int send_target(const char *topic,const char *body,bool retained){
 assert(topic_index<4 && !strcmp(topic,expected_topics[topic_index++]));
 assert(body && !retained);return MQTT_SUCCESS;
}
static void test_targets(void){
 mqtt_account_t original[4];char user[64];memset(user,'x',63);user[63]=0;
 const char *models[]={"a1","m1","tc1","tc1"};
 for(unsigned i=0;i<4;i++){
  assert(mqtt_account_init(&original[i],i==3?0:1,models[i],user,"AABBCCDDEEFF"));
  scope=original[i];assert(mqtt_leaf(&scope,"result",expected_topics[i],sizeof(expected_topics[i])));
  switch_scope=true;
  assert(service_mqtt_input(send_target,&scope,"{}",2,false)==MQTT_SUCCESS);
 }
 /* Every slot must own the destination from before command execution. */
 memset(&scope,0,sizeof(scope));topic_index=0;
 assert(service_mqtt_flush(send_target)==MQTT_SUCCESS && topic_index==4);
 assert(!service_mqtt_reply_count);
 for(unsigned i=0;i<4;i++)assert(service_mqtt_input(send_target,&original[i],"{}",2,false)==MQTT_SUCCESS);
 service_mqtt_clear();assert(!service_mqtt_reply_count);
 puts("Compact reply targets: long usernames, mixed models/accounts, legacy TC1 and command account mutation passed");
}
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
 test_owned_wire();
 test_targets();
 executions=0;
 assert(mqtt_account_init(&scope,1,"m1","test","aabbccddeeff"));
 assert(service_mqtt_input(send_reply,&scope,"{}",2,true)==0);assert(!executions);
 assert(service_mqtt_input(send_reply,&scope,"{}",2,false)==0);assert(!calls);
 nested=true;assert(service_mqtt_flush(send_reply)==0);assert(calls==1 && service_mqtt_reply_count==1);
 assert(service_mqtt_flush(send_reply)==0);assert(calls==2 && !service_mqtt_reply_count);
 for(unsigned i=0;i<4;i++)assert(service_mqtt_input(send_reply,&scope,"{}",2,false)==0);
 unsigned before=executions;assert(service_mqtt_input(send_reply,&scope,"{}",2,false)==-1);assert(executions==before);
 service_mqtt_clear();assert(!service_mqtt_reply_count);
 (void)service_mqtt_snapshot;(void)service_mqtt_capabilities;
 puts("MQTT result callbacks defer publication, bound the queue and clear stale replies");
 puts("MQTT owned JSON graphs released before sender; serialization/send failures clean up");
}

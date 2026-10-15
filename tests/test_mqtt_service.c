#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "json.h"
#include "../common/mqtt_account.h"
#define MQTT_SUCCESS 0
#define MQTT_FAILURE -1
#include "../common/mqtt_service.inc"
static unsigned calls, executions;
static bool nested;
static mqtt_account_t scope;
json_object *service_parse(const void *p,size_t n){(void)p;(void)n;return json_object_new_object();}
json_object *service_command(json_object *q){(void)q;executions++;return json_object_new_object();}
json_object *service_query(const char *a){(void)a;json_object *r=json_object_new_object();json_object_object_add(r,"data",json_object_new_object());return r;}
json_object *service_product_capabilities(void){return json_object_new_object();}
static int send_reply(const char *topic,const char *body,bool retained){
 assert(strstr(topic,"/result"));assert(body);assert(!retained);calls++;
 if(nested){nested=false;assert(service_mqtt_input(send_reply,&scope,"{}",2,false)==0);assert(calls==1);}
 return 0;
}
int main(void){
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
}

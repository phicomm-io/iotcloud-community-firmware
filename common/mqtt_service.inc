/* Included by the MQTT owner only; never call the socket from HTTP or a timer. */
#include "service.h"
static int service_mqtt_publish(int (*sender)(const char *,const char *,bool),const mqtt_account_t *scope,const char *leaf,json_object *o,bool retain){
 if(!o)return MQTT_FAILURE;char topic[MQTT_TOPIC_MAX];mqtt_leaf(scope,leaf,topic,sizeof(topic));int rc=sender(topic,json_object_to_json_string(o),retain);json_object_put(o);return rc;
}
static int service_mqtt_input(int (*sender)(const char *,const char *,bool),const mqtt_account_t *scope,const void *payload,size_t length,bool retained){
 if(retained)return MQTT_SUCCESS;json_object *q=service_parse(payload,length),*r=service_command(q);if(q)json_object_put(q);return service_mqtt_publish(sender,scope,"result",r,false);
}
static int service_mqtt_snapshot(int (*sender)(const char *,const char *,bool),const mqtt_account_t *scope){
 json_object *r=service_query("status.get");if(!r)return MQTT_FAILURE;json_object *data=json_object_object_get(r,"data");if(data)json_object_get(data);json_object_put(r);
 return service_mqtt_publish(sender,scope,"state",data,true);
}
static int service_mqtt_capabilities(int (*sender)(const char *,const char *,bool),const mqtt_account_t *scope){return service_mqtt_publish(sender,scope,"capabilities",service_product_capabilities(),true);}

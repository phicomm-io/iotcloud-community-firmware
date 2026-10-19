#include "ota/mode.h"
/* Included by the MQTT owner only; never call the socket from HTTP or a timer. */
#include "service.h"
#include "printbuf.h"
static struct printbuf *service_mqtt_wire(json_object *o){
 /* PUBACK waiting can dispatch callbacks or overlap an HTTP status request.
  * Keep only independent wire bytes while waiting, not the JSON object graph. */
 struct printbuf *wire=o?json_object_to_json_string_ex(o):NULL;
 if(o)json_object_put(o);
 return wire;
}
static int service_mqtt_send(int (*sender)(const char *,const char *,bool),const char *topic,json_object *o,bool retain){
 struct printbuf *wire=service_mqtt_wire(o);
 if(!wire)return MQTT_FAILURE;
 int rc=sender(topic,wire->buf,retain);printbuf_free(wire);return rc;
}
static int service_mqtt_publish(int (*sender)(const char *,const char *,bool),const mqtt_account_t *scope,const char *leaf,json_object *o,bool retain){
 if(!o)return MQTT_FAILURE;char topic[MQTT_TOPIC_MAX];mqtt_leaf(scope,leaf,topic,sizeof(topic));return service_mqtt_send(sender,topic,o,retain);
}
/* SDK publish waits for PUBACK by dispatching incoming callbacks. Publishing
 * a result from such a callback would consume the outer publish's ACK. Queue
 * replies and flush only from the owner loop, outside MQTT packet dispatch.
 * Serialize at enqueue so pending replies retain wire buffers, not up to four
 * full JSON graphs and their object hash tables while new commands execute. */
static struct { mqtt_target_t target; struct printbuf *wire; } service_mqtt_replies[4];
static unsigned service_mqtt_reply_count;
static void service_mqtt_clear(void){
 for(unsigned i=0;i<service_mqtt_reply_count;i++)printbuf_free(service_mqtt_replies[i].wire);
 service_mqtt_reply_count=0;
}
static int service_mqtt_flush(int (*sender)(const char *,const char *,bool)){
 unsigned count=service_mqtt_reply_count;
 while(count-- && service_mqtt_reply_count){
  char topic[MQTT_TOPIC_MAX];
  bool valid=mqtt_target_leaf(&service_mqtt_replies[0].target,"result",topic,sizeof(topic));
  struct printbuf *wire=service_mqtt_replies[0].wire;
  service_mqtt_reply_count--;
  memmove(service_mqtt_replies,service_mqtt_replies+1,service_mqtt_reply_count*sizeof(service_mqtt_replies[0]));
  if(!valid){printbuf_free(wire);return MQTT_FAILURE;}
  int rc=sender(topic,wire->buf,false);
  printbuf_free(wire);
  if(rc!=MQTT_SUCCESS)return rc;
 }
 return MQTT_SUCCESS;
}
static int service_mqtt_input(int (*sender)(const char *,const char *,bool),const mqtt_account_t *scope,const void *payload,size_t length,bool retained){
 (void)sender;
 if(ota_job_active() && length>1024)return MQTT_SUCCESS;
 if(retained)return MQTT_SUCCESS;
 if(service_mqtt_reply_count==4)return MQTT_FAILURE;
 mqtt_target_set(&service_mqtt_replies[service_mqtt_reply_count].target,scope);
 json_object *q=service_parse(payload,length),*r=service_command(q);if(q)json_object_put(q);
 struct printbuf *wire=service_mqtt_wire(r);
 if(!wire)return MQTT_FAILURE;
 service_mqtt_replies[service_mqtt_reply_count++].wire=wire;
 return MQTT_SUCCESS;
}
static int service_mqtt_snapshot(int (*sender)(const char *,const char *,bool),const mqtt_account_t *scope){
 int rc=service_mqtt_flush(sender);if(rc!=MQTT_SUCCESS)return rc;
 json_object *r=service_query("status.get");if(!r)return MQTT_FAILURE;json_object *data=json_object_object_get(r,"data");if(data)json_object_get(data);json_object_put(r);
 return service_mqtt_publish(sender,scope,"state",data,true);
}
static int service_mqtt_capabilities(int (*sender)(const char *,const char *,bool),const mqtt_account_t *scope){service_mqtt_clear();return service_mqtt_publish(sender,scope,"capabilities",service_product_capabilities(),true);}

#include "ota/mode.h"
/* Included by the MQTT owner only; never call the socket from HTTP or a timer. */
#include "service.h"
#include "printbuf.h"
#include "busy_reply.h"
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
 /* Telemetry allocation failure must not tear down an otherwise live socket. */
 struct printbuf *wire=service_mqtt_wire(o);if(!wire)return MQTT_SUCCESS;
 char topic[MQTT_TOPIC_MAX];mqtt_leaf(scope,leaf,topic,sizeof(topic));
 int rc=sender(topic,wire->buf,retain);printbuf_free(wire);return rc;
}
/* SDK publish waits for PUBACK by dispatching incoming callbacks. Publishing
 * a result from such a callback would consume the outer publish's ACK. Queue
 * replies and flush only from the owner loop, outside MQTT packet dispatch.
 * Serialize at enqueue so pending replies retain wire buffers, not up to four
 * full JSON graphs and their object hash tables while new commands execute. */
static struct { mqtt_target_t target; struct printbuf *wire; char busy[SERVICE_BUSY_BYTES]; } service_mqtt_replies[4];
static unsigned service_mqtt_reply_count;
static void service_mqtt_clear(void){
 for(unsigned i=0;i<service_mqtt_reply_count;i++)if(service_mqtt_replies[i].wire)printbuf_free(service_mqtt_replies[i].wire);
 service_mqtt_reply_count=0;
}
static int service_mqtt_flush(int (*sender)(const char *,const char *,bool)){
 unsigned count=service_mqtt_reply_count;
 while(count-- && service_mqtt_reply_count){
  char topic[MQTT_TOPIC_MAX];
  bool valid=mqtt_target_leaf(&service_mqtt_replies[0].target,"result",topic,sizeof(topic));
  struct printbuf *wire=service_mqtt_replies[0].wire;
  char busy[SERVICE_BUSY_BYTES];
  if(!wire)memcpy(busy,service_mqtt_replies[0].busy,sizeof(busy));
  service_mqtt_reply_count--;
  memmove(service_mqtt_replies,service_mqtt_replies+1,service_mqtt_reply_count*sizeof(service_mqtt_replies[0]));
  if(!valid){if(wire)printbuf_free(wire);return MQTT_FAILURE;}
  int rc=sender(topic,wire?wire->buf:busy,false);
  if(wire)printbuf_free(wire);
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
 unsigned claim=service_memory_claim(length);
 struct printbuf *wire=NULL;
 if(claim){
  uint64_t failures=service_memory_failures();
  json_object *q=service_parse(payload,length);
  json_object *r=service_memory_failures()==failures?service_command(q):NULL;if(q)json_object_put(q);
  wire=service_mqtt_wire(r);service_memory_release(claim);
 }
 if(!wire)service_busy_wire(payload,length,service_mqtt_replies[service_mqtt_reply_count].busy);
 service_mqtt_replies[service_mqtt_reply_count++].wire=wire;
 return MQTT_SUCCESS;
}
static int service_mqtt_snapshot(int (*sender)(const char *,const char *,bool),const mqtt_account_t *scope){
 int rc=service_mqtt_flush(sender);if(rc!=MQTT_SUCCESS)return rc;
 unsigned claim=service_memory_claim(0);if(!claim)return MQTT_SUCCESS;
 json_object *r=service_query("status.get");if(!r){service_memory_release(claim);return MQTT_SUCCESS;}json_object *data=json_object_object_get(r,"data");if(data)json_object_get(data);json_object_put(r);
 rc=service_mqtt_publish(sender,scope,"state",data,true);service_memory_release(claim);return rc;
}
static int service_mqtt_capabilities(int (*sender)(const char *,const char *,bool),const mqtt_account_t *scope){
 service_mqtt_clear();unsigned claim=service_memory_claim(0);if(!claim)return MQTT_SUCCESS;
 int rc=service_mqtt_publish(sender,scope,"capabilities",service_product_capabilities(),true);
 service_memory_release(claim);return rc;
}

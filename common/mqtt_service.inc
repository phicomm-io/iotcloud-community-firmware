#include "ota/mode.h"
/* Included by the MQTT owner only; never call the socket from HTTP or a timer. */
#include "service.h"
#include "printbuf.h"
#include "json_owned.h"
#include "busy_reply.h"
#include "json_heap.h"
#include "mico.h"
static struct printbuf *service_mqtt_wire(json_object *o){
 /* PUBACK waiting can dispatch callbacks or overlap an HTTP status request.
  * Keep only independent wire bytes while waiting, not the JSON object graph. */
 return json_owned_to_wire(o);
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
/* Never publish from a packet callback: a nested QoS1 publish can consume
 * the outer PUBACK. Normal reply metadata is allocated against the live heap,
 * not a fixed count. Four compact emergency entries can carry busy IDs when
 * even metadata allocation is denied. They retain no payload or JSON graph. */
typedef struct service_mqtt_reply {
 struct service_mqtt_reply *next;
 mqtt_target_t target;
 uint32_t queued_at;
 unsigned char emergency;
 bool busy;
 union { struct printbuf *wire; char id[65]; } body;
} service_mqtt_reply_t;
static service_mqtt_reply_t service_mqtt_emergency[4];
static unsigned service_mqtt_emergency_used;
static service_mqtt_reply_t *service_mqtt_head,*service_mqtt_tail;
static unsigned service_mqtt_reply_count;
static void service_mqtt_entry_free(service_mqtt_reply_t *entry){
 if(entry->emergency)service_mqtt_emergency_used&=~(1u<<(entry->emergency-1));
 else json_heap_free(entry);
}
static void service_mqtt_clear(void){
 while(service_mqtt_head){
  service_mqtt_reply_t *entry=service_mqtt_head;service_mqtt_head=entry->next;
  if(!entry->busy)printbuf_free(entry->body.wire);
  service_mqtt_entry_free(entry);
 }
 service_mqtt_tail=NULL;service_mqtt_reply_count=0;
 service_memory_mqtt(0,false,0);
}
static int service_mqtt_flush(int (*sender)(const char *,const char *,bool)){
 /* One bounded pass: new callbacks cannot keep the owner here forever. */
 unsigned count=service_mqtt_reply_count;
 while(count-- && service_mqtt_head){
  service_mqtt_reply_t *entry=service_mqtt_head;
  char topic[MQTT_TOPIC_MAX],busy[SERVICE_BUSY_BYTES];
  bool valid=mqtt_target_leaf(&entry->target,"result",topic,sizeof(topic));
  struct printbuf *wire=entry->busy?NULL:entry->body.wire;
  if(entry->busy)service_busy_format(entry->body.id,busy);
  service_mqtt_head=entry->next;if(!service_mqtt_head)service_mqtt_tail=NULL;
  service_mqtt_reply_count--;
  uint32_t delay=(uint32_t)(mico_rtos_get_time()-entry->queued_at);
  /* Release metadata/emergency position before sender dispatches callbacks. */
  service_mqtt_entry_free(entry);
  service_memory_mqtt(service_mqtt_reply_count,false,delay);
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
 service_mqtt_reply_t *entry=json_heap_malloc(sizeof(*entry));
 bool emergency=!entry;
 if(!entry){
  for(unsigned i=0;i<4;i++)if(!(service_mqtt_emergency_used&(1u<<i))){
   entry=&service_mqtt_emergency[i];memset(entry,0,sizeof(*entry));
   entry->emergency=i+1;service_mqtt_emergency_used|=1u<<i;break;
  }
  if(!entry){service_memory_mqtt(service_mqtt_reply_count,true,0);return MQTT_FAILURE;}
 }else memset(entry,0,sizeof(*entry));
 /* Own the destination before a command can change account configuration. */
 mqtt_target_set(&entry->target,scope);entry->queued_at=mico_rtos_get_time();
 unsigned claim=emergency?0:service_memory_claim(length);
 struct printbuf *wire=NULL;
 if(claim){
  uint64_t failures=service_memory_failures();
  json_object *q=service_parse(payload,length);
  json_object *r=service_memory_failures()==failures?service_command(q):NULL;if(q)json_object_put(q);
  wire=service_mqtt_wire(r);service_memory_release(claim);
 }
 entry->busy=!wire;
 if(wire)entry->body.wire=wire;else service_busy_id(payload,length,entry->body.id);
 if(service_mqtt_tail)service_mqtt_tail->next=entry;else service_mqtt_head=entry;
 service_mqtt_tail=entry;service_mqtt_reply_count++;
 service_memory_mqtt(service_mqtt_reply_count,false,0);
 return MQTT_SUCCESS;
}
static int service_mqtt_snapshot(int (*sender)(const char *,const char *,bool),const mqtt_account_t *scope){
 int rc=service_mqtt_flush(sender);if(rc!=MQTT_SUCCESS)return rc;
 /* Callbacks during flush may have queued fresh results. Defer telemetry. */
 if(service_mqtt_reply_count)return MQTT_SUCCESS;
 unsigned claim=service_memory_claim(0);if(!claim)return MQTT_SUCCESS;
 json_object *r=service_query("status.get");if(!r){service_memory_release(claim);return MQTT_SUCCESS;}json_object *data=json_object_object_get(r,"data");if(data)json_object_get(data);json_object_put(r);
 rc=service_mqtt_publish(sender,scope,"state",data,true);service_memory_release(claim);return rc;
}
static int service_mqtt_capabilities(int (*sender)(const char *,const char *,bool),const mqtt_account_t *scope){
 service_mqtt_clear();unsigned claim=service_memory_claim(0);if(!claim)return MQTT_SUCCESS;
 int rc=service_mqtt_publish(sender,scope,"capabilities",service_product_capabilities(),true);
 service_memory_release(claim);return rc;
}

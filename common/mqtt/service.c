#include "../ota/mode.h"
/* Compiled once, shared by all model adapters. */
#include "service.h"
#include "client.h"
#include "../service.h"
#include "printbuf.h"
#include "../json_owned.h"
#include "../busy_reply.h"
#include "json_heap.h"
#include "mico.h"
static struct printbuf *service_mqtt_wire(json_object *o){
 /* PUBACK waiting can dispatch callbacks or overlap an HTTP status request.
  * Keep only independent wire bytes while waiting, not the JSON object graph. */
 return json_owned_to_wire(o);
}
int service_mqtt_send(int (*sender)(const char *,const char *,bool),const char *topic,json_object *o,bool retain){
 struct printbuf *wire=service_mqtt_wire(o);
 if(!wire)return MQTT_FAILURE;
 int rc=sender(topic,wire->buf,retain);printbuf_free(wire);return rc;
}
__attribute__((noinline))
static int service_mqtt_publish(int (*sender)(const char *,const char *,bool),const mqtt_account_t *scope,const char *leaf,json_object *o,bool retain,unsigned claim,unsigned json_claim){
 /* Telemetry allocation failure must not tear down an otherwise live socket. */
 struct printbuf *wire=service_mqtt_wire(o);
 service_json_release(json_claim);service_memory_release(claim);
 if(!wire)return MQTT_SUCCESS;
 char topic[MQTT_TOPIC_MAX];mqtt_leaf(scope,leaf,topic,sizeof(topic));
 int rc=sender(topic,wire->buf,retain);printbuf_free(wire);return rc;
}
/* Commands are invoked by the shared owner only after Paho has unwound.
 * Exactly one result wire exists; no response graph/linked backlog crosses a
 * transport callback. The caller holds its receipt credit through this send. */
static bool service_mqtt_capabilities_pending;
void service_mqtt_clear(void) {
 service_mqtt_capabilities_pending=false;
 ph_mqtt_pending_clear();
}
int service_mqtt_flush(int (*sender)(const char *,const char *,bool)) {
 (void)sender;return ph_mqtt_poll(NULL);
}
int service_mqtt_input(int (*sender)(const char *,const char *,bool),const mqtt_account_t *scope,const void *payload,size_t length,bool retained) {
 if(!ph_mqtt_owner() || !ph_mqtt_debug_core()->dispatch_depth)return MQTT_FAILURE;
 if(retained || (ota_job_active() && length>1024))return MQTT_SUCCESS;
 mqtt_target_t target;mqtt_target_set(&target,scope);
 char busy[SERVICE_BUSY_BYTES];service_busy_wire(payload,length,busy);
 unsigned token=service_json_claim();
 unsigned claim=token?service_memory_claim(length):0;
 struct printbuf *wire=NULL;
 uint64_t failures=service_memory_failures();
 json_object *q=claim?service_parse(payload,length):NULL;
 /* Everything borrowed is now copied (including the fixed failure wire).
  * Do not use payload/length/MessageData again after consuming the entry. */
 bool admitted=ph_mqtt_input_consumed();
 json_object *r=claim && admitted && service_memory_failures()==failures?service_command(q):NULL;
 if(q)json_object_put(q);
 wire=service_mqtt_wire(r);
 service_memory_release(claim);service_json_release(token);
 char topic[MQTT_TOPIC_MAX];
 if(!mqtt_target_leaf(&target,"result",topic,sizeof(topic))) { if(wire)printbuf_free(wire);return MQTT_FAILURE; }
 int rc=sender(topic,wire?wire->buf:busy,false);
 if(wire)printbuf_free(wire);
 return rc;
}
/* A busy graph builder must not permanently skip initial capabilities. The
 * owner retries on its next snapshot; no lock/lease crosses the sender. */
__attribute__((noinline))
static int service_mqtt_try_capabilities(int (*sender)(const char *,const char *,bool),const mqtt_account_t *scope){
 unsigned token=service_json_claim();if(!token)return MQTT_SUCCESS;
 unsigned claim=service_memory_claim(0);if(!claim){service_json_release(token);return MQTT_SUCCESS;}
 struct printbuf *wire=service_mqtt_wire(service_product_capabilities());
 service_json_release(token);service_memory_release(claim);
 if(!wire)return MQTT_SUCCESS;
 char topic[MQTT_TOPIC_MAX];mqtt_leaf(scope,"capabilities",topic,sizeof(topic));
 int rc=sender(topic,wire->buf,true);
 if(rc==MQTT_SUCCESS)service_mqtt_capabilities_pending=false;printbuf_free(wire);return rc;
}
__attribute__((noinline))
int service_mqtt_snapshot(int (*sender)(const char *,const char *,bool),const mqtt_account_t *scope){
 int rc=MQTT_SUCCESS;
 /* One command was serviced above. A stream of further commands must not
  * starve this bounded periodic snapshot indefinitely. */
 if(service_mqtt_capabilities_pending){
  rc=service_mqtt_try_capabilities(sender,scope);if(rc!=MQTT_SUCCESS)return rc;
  if(service_mqtt_capabilities_pending)return MQTT_SUCCESS;
 }
 unsigned json_claim=service_json_claim();if(!json_claim)return MQTT_SUCCESS;
 unsigned claim=service_memory_claim(0);if(!claim){service_json_release(json_claim);return MQTT_SUCCESS;}
 json_object *r=service_query("status.get");if(!r){service_memory_release(claim);service_json_release(json_claim);return MQTT_SUCCESS;}json_object *data=json_object_object_get(r,"data");if(data)json_object_get(data);json_object_put(r);
 return service_mqtt_publish(sender,scope,"state",data,true,claim,json_claim);
}
int service_mqtt_capabilities(int (*sender)(const char *,const char *,bool),const mqtt_account_t *scope){
 service_mqtt_capabilities_pending=true;
 return service_mqtt_try_capabilities(sender,scope);
}

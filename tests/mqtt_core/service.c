/* Actual service/digest/owned encoder, invoked only after the real pinned Paho
 * callback returns. The transport below is the in-memory protocol fixture. */
#define REAL_SERVICE 1
#define main protocol_fixture_main
#include "paho.c"
#undef main
#include "../../common/service.c"
#include "../../common/mqtt/service.h"
#include "json_heap.h"
static unsigned mutations;
static bool rename_target;
static bool large_receipt;
static unsigned large_receipt_writes;
static unsigned status_mode,status_builds;
json_object *automation_command(const char*a,json_object*p,const char**c,bool*h){(void)a;(void)p;(void)c;*h=false;return NULL;}
json_object *service_product_capabilities(void){return json_object_new_object();}
json_object *service_product(const char*a,json_object*p,const char**code){
 (void)p;*code="ok";
 if(!strcmp(a,"fan.set")){mutations++;if(rename_target)strcpy(account.base,"renamed");}
 json_object *out=json_object_new_object();
 if(large_receipt && !strcmp(a,"fan.set")){
  char padding[1025];memset(padding,'x',sizeof(padding)-1);padding[sizeof(padding)-1]=0;
  json_object_object_add(out,"padding",json_object_new_string(padding));
 }
 if(!strcmp(a,"status.get")){
  status_builds++;
  if(status_mode==1)*code="busy";
  if(status_mode!=2)json_object_object_add(out,"model",json_object_new_string("a1"));
 }
 return out;
}
static int sender(const char *topic,const char *body,bool retain){
 assert(!ph_mqtt_debug_core()->protocol_depth);
 assert(!ph_mqtt_debug_core()->inflight[PH_MQTT_COMMAND]);
 MQTTMessage m={0};m.qos=1;m.retained=retain;m.payload=(void*)body;m.payloadlen=strlen(body);
 return ph_mqtt_publish(&client,topic,&m);
}
static void real_handler(MessageData*d){
 (void)service_mqtt_input(sender,&account,d->message->payload,d->message->payloadlen,d->message->retained);
}
static void clear_receipts(void){
 for(unsigned i=0;i<8;i++){json_heap_free(receipts[i].response);memset(&receipts[i],0,sizeof(receipts[i]));}
 receipt_next=0;
}
static int service_write_packet(Network*n,unsigned char*p,int count,int timeout){
 if(large_receipt && (p[0]>>4)==PUBLISH){
  const ph_mqtt_core *core=ph_mqtt_debug_core();
  /* This is the actual SDK buffer while a real command receipt is sent. */
  assert(core->dispatch_depth==1 && core->protocol_depth==1);
  assert(count>512 && client.buf_size>512 && client.buf_size>=(size_t)count);
  assert(((allocation*)client.buf-1)->entry.size==client.buf_size);
  large_receipt_writes++;
 }
 return write_packet(n,p,count,timeout);
}
static void start(void){
 rx_at=rx_size=tx_size=0;network.mqttread=read_packet;network.mqttwrite=service_write_packet;
 assert(ph_mqtt_client_init(&client,&network,50)==0);client.isconnected=1;
 client.keepAliveInterval=30;countdown(&client.ping_timer,30);
 snprintf(injected_topic,sizeof(injected_topic),"%s/command",account.base);
 assert(ph_mqtt_subscribe(&client,injected_topic,QOS1,handler)==0);
}
static int command(const char *wire){
 incoming(injected_topic,wire,15);int rc=ph_mqtt_yield(&client,1);
 if(rc==0)rc=ph_mqtt_poll(&client);return rc;
}
static void finish(void){
 fail_allocation=0;assert(ph_mqtt_client_deinit(&client)==0);
 ph_mqtt_diagnostics d;ph_mqtt_snapshot(&d);
 assert(!d.bytes&&!d.credit&&!d.inflight[0]&&!d.inflight[1]&&!d.queued[0]&&!d.queued[1]);
}
#ifdef LARGE_RECEIPT_FIXTURE
int main(void){
 service_init();assert(mqtt_account_init(&account,1,"a1","test","aabbccddeeff"));
 assert(ph_mqtt_owner_init(&account,8192,&thread,accept));
 heap_capacity=64000;start();assert(client.buf_size==512);large_receipt=true;
 const char *request="{\"action\":\"fan.set\",\"request_id\":\"large-result\",\"params\":{}}";
 assert(command(request)==0);
 assert(mutations==1 && large_receipt_writes==1 && strlen(last_body)>1024);
 assert(strstr(last_body,"\"padding\"") && strstr(last_body,"\"ok\""));
 assert(client.buf_size==512 && ((allocation*)client.buf-1)->entry.size==512);
 assert(!ph_mqtt_receipt_pending() && !ph_mqtt_debug_core()->credit);
 assert(command(request)==0 && mutations==1 && large_receipt_writes==2);
 finish();clear_receipts();assert(!native_live&&!charged_live);
 puts("real service >1KiB receipt: dispatch TX growth, PUBACK, true capacity, shrink and replay PASS");
}
#elif defined(PREFLIGHT_FIXTURE)
#include "preflight_service.in"
#elif defined(A1_BUILDERS_FIXTURE)
#include "a1_builders.inc"
#else
int main(void){
 service_init();assert(mqtt_account_init(&account,1,"a1","test","aabbccddeeff"));
 assert(ph_mqtt_owner_init(&account,8192,&thread,accept));
 char request[8193];const char *control="{\"action\":\"fan.set\",\"request_id\":\"large\",\"params\":{}}";
 strcpy(request,control);memset(request+strlen(control),' ',8192-strlen(control));request[8192]=0;
 heap_capacity=64000;start();
 /* Busy and incomplete product captures must preserve the previous retained
  * state; they are retryable work, never a successful empty publication. */
 for(status_mode=1;status_mode<=2;status_mode++){
  now_ms+=1000;
  unsigned before=publications;
  assert(service_mqtt_snapshot(sender,&account)==PH_MQTT_MORE);
  assert(publications==before&&!json_owner);
  service_memory_state_t m;service_memory_snapshot(&m);assert(!m.active&&!m.reserved);
 }
 /* A failed full capture is capped to one attempt per second across wrap;
  * callback dispatch remains available throughout the delay. */
 now_ms=UINT32_MAX-500;status_mode=1;
 unsigned captures=status_builds;
 for(unsigned i=0;i<250;i++){assert(service_mqtt_snapshot(sender,&account)==PH_MQTT_MORE);now_ms+=20;}
 assert(status_builds-captures==5 && !json_owner);
 assert(command("{\"action\":\"fan.set\",\"request_id\":\"during-snapshot-wait\",\"params\":{}} ")==0);
 assert(mutations==1);mutations=0;clear_receipts();
 status_mode=0;now_ms+=1000;unsigned before=publications;
 assert(service_mqtt_snapshot(sender,&account)==0&&publications==before+1);
 assert(strstr(last_body,"a1"));finish();clear_receipts();assert(!native_live);
 start();rename_target=true;
 char target[512];snprintf(target,sizeof(target),"%s/result",account.base);
 assert(command(request)==0);assert(mutations==1);assert(!strcmp(target,last_topic));assert(strstr(last_body,"\"ok\""));
 assert(command(request)==0&&mutations==1);assert(strstr(last_body,"\"ok\""));
 assert(command("{\"action\":\"fan.set\",\"request_id\":\"large\",\"params\":{\"on\":true}} ")==0);
 assert(mutations==1&&strstr(last_body,"conflict"));finish();clear_receipts();assert(!native_live);
 rename_target=false;assert(mqtt_account_init(&account,1,"a1","test","aabbccddeeff"));
 /* Exact modern topic, actual Paho callback overload: four deferred entries,
  * two fixed busy receipts, then visible refusal without running the drop. */
 start();unsigned busy_before=busy_publications,mutation_before=mutations;
 for(unsigned i=0;i<6;i++)incoming(injected_topic,control,100+i);
 assert(ph_mqtt_yield(&client,1)==0);assert(mutations==mutation_before);
 ph_mqtt_diagnostics saturated;ph_mqtt_snapshot(&saturated);assert(saturated.queued[0]==4);
 snprintf(target,sizeof(target),"%s/result",account.base);
 assert(ph_mqtt_poll(&client)==0&&busy_publications==busy_before+2);
 assert(!strcmp(target,busy_target));assert(mutations==mutation_before+1);
 for(unsigned i=0;i<3;i++)assert(ph_mqtt_poll(&client)==0);
 assert(mutations==mutation_before+1);finish();clear_receipts();assert(!native_live);
 start();mutation_before=mutations;ph_mqtt_diagnostics prior;ph_mqtt_snapshot(&prior);
 const char *dropped="{\"action\":\"fan.set\",\"request_id\":\"retry-after-drop\",\"params\":{}}";
 for(unsigned i=0;i<6;i++)incoming(injected_topic,control,200+i);
 incoming(injected_topic,dropped,206);
 assert(ph_mqtt_yield(&client,1)==0&&mutations==mutation_before);
 ph_mqtt_snapshot(&saturated);
 assert(saturated.ingress_dropped==prior.ingress_dropped+1&&saturated.busy_overflow==prior.busy_overflow+1);
 assert(saturated.errors==prior.errors&&saturated.generation==prior.generation);
 for(unsigned i=0;i<4;i++)assert(ph_mqtt_poll(&client)==0);
 assert(mutations==mutation_before+1); /* only the admitted shared ID ran */
 assert(command(dropped)==0&&mutations==mutation_before+2);
 assert(command(dropped)==0&&mutations==mutation_before+2); /* same-ID replay */
 finish();clear_receipts();assert(!native_live);
 /* ID-less overload still never executes rejected messages. Admitted ID-less
  * controls deliberately have no dedup guarantee: four admitted means four runs. */
 start();mutation_before=mutations;ph_mqtt_snapshot(&prior);
 const char *anonymous="{\"action\":\"fan.set\",\"params\":{}}";
 for(unsigned i=0;i<7;i++)incoming(injected_topic,anonymous,300+i);
 assert(!ph_mqtt_yield(&client,1)&&mutations==mutation_before);
 ph_mqtt_snapshot(&saturated);assert(saturated.ingress_dropped==prior.ingress_dropped+1);
 assert(saturated.busy_overflow==prior.busy_overflow+1);
 for(unsigned i=0;i<4;i++)assert(!ph_mqtt_poll(&client));
 assert(mutations==mutation_before+4);finish();clear_receipts();assert(!native_live);
 /* Every native allocation boundary across Paho RX/input/JSON/digest/receipt/
  * encoder/TX gets a failure. Reconnect/replay must never repeat a mutation. */
 unsigned exercised=0;
 for(unsigned point=1;point<=100;point++){
  clear_receipts();mutations=0;start();unsigned before=native_allocs;
  fail_allocation=before+point;
  (void)command(control);bool ran=mutations!=0;exercised+=native_allocs>=fail_allocation;
  finish();unsigned performed=mutations;
  start();assert(command(control)==0);assert(mutations==(ran?performed:1));
  assert(strstr(last_body,"ok")||strstr(last_body,"busy"));finish();clear_receipts();assert(!native_live);
 }
 assert(exercised>30);
 heap_capacity=28364;start();unsigned held=service_json_claim();assert(held);assert(command(control)==0);assert(strstr(last_body,"busy"));service_json_release(held);assert(command("{\"action\":\"status.get\",\"params\":{}}")==0);finish();clear_receipts();assert(!native_live);
 printf("actual shared MQTT service: 8192B input, original rename target, digest replay/conflict and %u allocation failures PASS; requested peak=%zu, charged peak=%zu\n",exercised,native_peak,charged_peak);
}

#endif

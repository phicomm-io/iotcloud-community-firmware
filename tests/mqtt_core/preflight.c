/* Real Paho in-memory protocol, owned queue and bounded preflight scheduler. */
#define OVERLOAD_FIXTURE
#define main existing_paho_scenarios
#include "paho.c"
#undef main
static unsigned busy_sent;
static void overload_handler(MessageData *d) { (void)d;executions++; }
static void overload_publication(const unsigned char *t,unsigned tn,const unsigned char *b,size_t n) {
 (void)t;(void)tn;if(memmem(b,n,"\"busy\"",6))busy_sent++;
}
static ph_mqtt_diagnostics snap(void){ph_mqtt_diagnostics d;ph_mqtt_snapshot(&d);return d;}
static int text(const char *topic,const char *body){MQTTMessage m={0};m.qos=QOS1;m.retained=1;m.payload=(void*)body;m.payloadlen=strlen(body);return ph_mqtt_publish(&client,topic,&m);}
static void fresh(void){heap_capacity=64000;fail_send=false;init();ph_mqtt_session_begin();}
static void finish(void){heap_capacity=64000;cleanup();assert(!charged_live);}
static void native_wait(void){
 fresh();now_ms=UINT32_MAX-500;
 unsigned sent=publications,allocs=native_allocs;ph_mqtt_diagnostics before=snap();
 heap_capacity=charged_live+16416;
 assert(text("native/large","original")==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED);
 for(unsigned i=0;i<99;i++){now_ms+=10;assert(ph_mqtt_resource_ready()==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED);}
 assert(native_allocs==allocs && publications==sent && snap().errors==before.errors);
 /* Bounded protocol work continues while all publications are throttled. */
 heap_capacity=64000;unsigned ran=executions;incoming("rx/command","{}",100);assert(!ph_mqtt_yield(&client,1));
 assert(ph_mqtt_poll(&client)==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED && executions==ran+1);
 ph_mqtt_resource_round_end(false);
 now_ms+=1000;heap_capacity=64000;
 assert(!text("native/small","x"));ph_mqtt_resource_round_end(true);
 /* A different native topic must not clear the original wait/deadline. */
 heap_capacity=charged_live+16416;
 assert(text("native/large","original")==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED);
 ph_mqtt_resource_round_end(false);now_ms+=1000;heap_capacity=64000;
 assert(!text("native/large","updated retained sample"));ph_mqtt_resource_round_end(true);
 heap_capacity=charged_live+16416;
 assert(text("native/new","new episode")==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED);
 now_ms+=29999;assert(ph_mqtt_resource_ready()==0);
 now_ms++;assert(ph_mqtt_resource_ready()==MQTT_FAILURE);
 assert(snap().failure.reason==PH_MQTT_CAUSE_RESOURCE_EXPIRED && publications==sent+2);
 assert(!snap().failure.read.requested&&!snap().failure.write.requested);
 finish();
}
static void external_wait(void){
 fresh();char large[2400];memset(large,'z',sizeof(large)-1);large[sizeof(large)-1]=0;
 assert(!ph_mqtt_enqueue_send("external/large",large,strlen(large),QOS1,true));
 ph_mqtt_diagnostics queued=snap();heap_capacity=charged_live+17000;
 unsigned sent=publications;
 assert(ph_mqtt_poll(&client)==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED);
 ph_mqtt_diagnostics waiting=snap();assert(waiting.inflight[1]==1&&!waiting.queued[1]);
 assert(waiting.bytes==queued.bytes && waiting.bytes<=PH_MQTT_SEND_BYTES);
 unsigned allocs=native_allocs;
 for(unsigned i=0;i<50;i++){now_ms+=10;assert(ph_mqtt_poll(&client)==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED);}
 assert(native_allocs==allocs && publications==sent);
 /* In-flight retained value cannot be replaced/coalesced by a new producer. */
 heap_capacity=64000;assert(!ph_mqtt_enqueue_send("external/large","new",3,QOS1,true));
 assert(snap().inflight[1]==1 && snap().queued[1]==1);
 now_ms+=1000;assert(!ph_mqtt_poll(&client));assert(publications==sent+1);
 assert(!ph_mqtt_poll(&client));assert(publications==sent+2 && !snap().bytes);
 finish();
 /* A small successful native packet every second cannot hide a blocked SEND. */
 fresh();assert(!ph_mqtt_enqueue_send("external/large",large,strlen(large),QOS1,false));
 heap_capacity=charged_live+17000;uint32_t began=now_ms;
 assert(ph_mqtt_poll(&client)==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED);
 for(unsigned i=1;i<30;i++){
  now_ms=began+i*1000;assert(!text("small","x"));
  ph_mqtt_resource_round_end(true);
  assert(ph_mqtt_poll(&client)==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED);
  assert(snap().inflight[1]==1 && snap().bytes==queued.bytes);
 }
 now_ms=began+30000;assert(ph_mqtt_poll(&client)==MQTT_FAILURE);
 assert(snap().failure.reason==PH_MQTT_CAUSE_RESOURCE_EXPIRED);finish();
 /* Stop/deinit must release a pending in-flight allocation before retry. */
 fresh();assert(!ph_mqtt_enqueue_send("external/large",large,strlen(large),QOS1,false));
 heap_capacity=charged_live+17000;assert(ph_mqtt_poll(&client)==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED);
 ph_mqtt_pending_clear();assert(!snap().bytes&&!snap().inflight[1]);finish();
}
static void emergency_wait(void){
 fresh();char topic[512];mqtt_leaf(&account,"command",topic,sizeof(topic));
 assert(!ph_mqtt_subscribe(&client,topic,QOS1,handler));
 for(unsigned i=0;i<6;i++)incoming(topic,"{\"request_id\":\"busy-identity\"}",200+i);
 for(unsigned i=0;i<6;i++)assert(!ph_mqtt_yield(&client,1));assert(snap().queued[0]==4);
 heap_capacity=charged_live+16416;unsigned busy=busy_sent,ran=executions;
 assert(ph_mqtt_poll(&client)==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED);
 assert(busy_sent==busy && executions==ran);
 for(unsigned i=0;i<50;i++){now_ms+=10;assert(ph_mqtt_poll(&client)==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED);}
 heap_capacity=64000;now_ms+=1000;
 assert(!ph_mqtt_poll(&client) && busy_sent==busy+2 && executions==ran+1);
 for(unsigned i=0;i<3;i++)assert(!ph_mqtt_poll(&client));
 assert(!ph_mqtt_work_pending());finish();
}
static void busy_capacity_wait(void){
 fresh();char topic[512],body[2048],external[4096];mqtt_leaf(&account,"command",topic,sizeof(topic));
 assert(!ph_mqtt_subscribe(&client,topic,QOS1,handler));
 size_t n=2048-sizeof(ph_mqtt_item)-strlen(topic)-2-16;
 memset(body,' ',n);memcpy(body,"{\"request_id\":\"capacity\"}",25);body[n]=0;
 for(unsigned i=0;i<4;i++)incoming(topic,body,700+i);
 for(unsigned i=0;i<4;i++)assert(!ph_mqtt_yield(&client,1));
 n=4096-sizeof(ph_mqtt_item)-strlen("external/capacity")-2-16;
 memset(external,'x',n);external[n]=0;
 assert(!ph_mqtt_enqueue_send("external/capacity",external,n,QOS1,false));
 unsigned ran=executions,busy=busy_sent;ph_mqtt_diagnostics queued=snap();
 assert(queued.bytes==PH_MQTT_QUEUE_BYTES && queued.queued[0]==4);
 heap_capacity=charged_live+16416;
 assert(ph_mqtt_poll(&client)==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED);
 assert(ph_mqtt_receipt_pending()&&snap().inflight[0]==1&&!snap().credit);
 assert(snap().bytes==queued.bytes&&executions==ran&&busy_sent==busy);
 now_ms+=1000;heap_capacity=64000;assert(!ph_mqtt_poll(&client));
 assert(!ph_mqtt_receipt_pending()&&!snap().inflight[0]&&!snap().credit);
 assert(executions==ran&&busy_sent==busy+1&&snap().queued[0]==3&&snap().queued[1]==1);
 finish();
}
static unsigned builds,session_case,stopped;static bool session_done,init_pending;static uint32_t low_since;
static int pressured_prepare(void*x,ph_mqtt_connection*s){(void)x;session_done=false;low_since=0;return prepare(NULL,s);}
static int pressured_initial(void*x){(void)x;builds++;if(session_case==2){heap_capacity=charged_live+16416;low_since=now_ms;}int rc=text("native/init","initial");init_pending=rc!=0;if(session_case==3){assert(!rc);heap_capacity=charged_live+16416;low_since=now_ms;}return rc;}
static int pressured_cycle(void*x){(void)x;builds++;if(init_pending){int rc=text("native/init","initial");if(!rc)init_pending=false;return rc;}return text("native/state","state");}
static bool pressured_stop(void*x){(void)x;
 if(low_since && (uint32_t)(now_ms-low_since)>=2100)heap_capacity=64000;
 return session_done;
}
static void pressured_closing(void*x,bool connected,int rc){(void)x;assert(connected);assert(rc==0||rc==PH_MQTT_RECONFIGURE);session_done=true;stopped++;}
static int bootstrap_read(Network*n,unsigned char*p,int c,int timeout){
 int r=read_packet(n,p,c,timeout);
 if(!low_since && c>1){heap_capacity=charged_live+16416;low_since=now_ms;}
 return r;
}
static int bootstrap_prepare(void*x,ph_mqtt_connection*s){int rc=pressured_prepare(x,s);s->read=bootstrap_read;return rc;}
static bool pressured_initial_ready(void*x){(void)x;return !init_pending;}
static unsigned event_ready_before,event_online_before;
static void pressured_event(void*x,unsigned e,int rc){(void)x;(void)rc;if(e==PH_MQTT_READY){assert(online_publications==event_online_before+1);assert((uint32_t)(now_ms-low_since)>=2100);ready_events++;session_done=true;}}
static void bootstrap_wait(void){
 for(session_case=1;session_case<=3;session_case++){
  heap_capacity=64000;builds=stopped=0;low_since=0;session_done=false;rx_at=rx_size=tx_size=0;
  event_ready_before=ready_events;event_online_before=online_publications;
  ph_mqtt_session_hooks h={NULL,session_case==1?bootstrap_prepare:pressured_prepare,subscription_plan,pressured_initial,pressured_cycle,pressured_closing,pressured_event,pressured_stop,pressured_initial_ready};
  ph_mqtt_session_run(&client,&h);
  assert(stopped==1 && ready_events==event_ready_before+1 && online_publications==event_online_before+1);
  /* A native initial failure must not rebuild at the 10ms owner cadence. */
  assert(builds<8 && !native_live && !charged_live);
 }
}
int main(void){
 assert(mqtt_account_init(&account,1,"a1","test","aabbccddeeff"));
 assert(ph_mqtt_owner_init(&account,8192,&thread,accept));
 native_wait();external_wait();emergency_wait();busy_capacity_wait();bootstrap_wait();
 fresh();heap_capacity=charged_live+16416;assert(publish()==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED);
 heap_capacity=64000;now_ms+=1000;fail_send=true;assert(publish()==MQTT_SOCKET_ERR);
 assert(snap().failure.reason==PH_MQTT_CAUSE_WRITE && ph_mqtt_poll(&client)==MQTT_FAILURE);
 fail_send=false;finish();ph_mqtt_owner_end();
 puts("preflight: native gate/wrap, SEND ownership/coalescing/expiry, emergency, bootstrap, real SDK failure PASS");
}

#include "mico.h"
#include "client.h"
#include "session.h"
#include "queue.h"
#include "MQTTPublish.h"
#include <assert.h>
#include <stdio.h>
#ifdef PH_EXPECTED_MESSAGE_HANDLERS
_Static_assert(MAX_MESSAGE_HANDLERS == PH_EXPECTED_MESSAGE_HANDLERS,
               "fixture must match the product SDK subscription capacity");
#endif
void *__real_malloc(size_t);void __real_free(void *);void *__real_realloc(void *,size_t);
typedef union { long double align; struct { size_t size,charge; } entry; } allocation;
static size_t native_live,native_peak,charged_live,charged_peak;static unsigned native_allocs,fail_allocation;
static size_t heap_capacity=40000;
void *__wrap_malloc(size_t size){
 if(++native_allocs==fail_allocation)return NULL;
 size_t charge=(size+sizeof(allocation)+15u)&~(size_t)15u;
 if(charge>heap_capacity || charged_live>heap_capacity-charge)return NULL;
 allocation *p=__real_malloc(sizeof(*p)+size);if(!p)return NULL;p->entry.size=size;
 p->entry.charge=charge;charged_live+=charge;if(charged_live>charged_peak)charged_peak=charged_live;
 native_live+=size;if(native_live>native_peak)native_peak=native_live;return p+1;
}
void __wrap_free(void *ptr){if(!ptr)return;allocation *p=(allocation*)ptr-1;assert(native_live>=p->entry.size&&charged_live>=p->entry.charge);native_live-=p->entry.size;charged_live-=p->entry.charge;__real_free(p);}
void *__wrap_realloc(void *ptr,size_t size){
 if(!ptr)return __wrap_malloc(size);if(!size){__wrap_free(ptr);return NULL;}
 void *q=__wrap_malloc(size);if(!q)return NULL;
 size_t old=((allocation*)ptr-1)->entry.size;memcpy(q,ptr,size<old?size:old);__wrap_free(ptr);return q;
}
void *__wrap_calloc(size_t count,size_t size){
 if(size && count>SIZE_MAX/size)return NULL;
 void *p=__wrap_malloc(count*size);if(p)memset(p,0,count*size);return p;
}
char *__wrap_strdup(const char *s){size_t n=strlen(s)+1;char*p=__wrap_malloc(n);if(p)memcpy(p,s,n);return p;}
static uint32_t now_ms;
static bool multi_session,multi_done;
static unsigned gate_mode,gate_online_before,gate_ready_before;
static bool gate_closed,gate_cancel,gate_current;static unsigned multi_attempt,multi_ready,multi_cycles;
static unsigned backoffs[32],backoff_count;
static uintptr_t current_thread=1;
static micoMemInfo_t heap={.free_memory=40000};
#ifdef PREFLIGHT_FIXTURE
static size_t preflight_memory(void);
#endif
int mico_rtos_init_mutex(mico_mutex_t *m){*m=0;return 0;}
void mico_rtos_lock_mutex(mico_mutex_t*m){assert(!*m);*m=1;}
void mico_rtos_unlock_mutex(mico_mutex_t*m){assert(*m);*m=0;}
bool mico_rtos_is_current_thread(mico_thread_t*t){return *t==current_thread;}
uint32_t mico_rtos_get_time(void){return now_ms;}
const micoMemInfo_t*MicoGetMemoryInfo(void){heap.free_memory=charged_live<heap_capacity?heap_capacity-charged_live:0;
#ifdef PREFLIGHT_FIXTURE
 size_t forced=preflight_memory();if(forced)heap.free_memory=forced;
#endif
 return &heap;}
static ph_mqtt_startup *pending_startup;
static uint32_t pending_generation;
void mico_thread_msleep(unsigned ms){now_ms+=ms;
 if(multi_session && ms>=1000){assert(backoff_count<32);backoffs[backoff_count++]=ms;}
 if(pending_startup){mico_thread_t *out=(mico_thread_t*)0;(void)out;
  ph_mqtt_startup_publish(pending_startup,pending_generation,true);pending_startup=NULL;}
}
char expired(Timer*t){return (int32_t)(now_ms-t->end_time)>=0;}
void countdown_ms(Timer*t,unsigned n){t->end_time=now_ms+n;}
void countdown(Timer*t,unsigned n){countdown_ms(t,n*1000);}
int left_ms(Timer*t){return expired(t)?0:(int)(t->end_time-now_ms);}
void InitTimer(Timer*t){memset(t,0,sizeof(*t));}
static unsigned char rx[65536],tx[20000];
static size_t rx_at,rx_size,tx_size;
static unsigned publications,executions,inject_count,write_fragment=65535;
static unsigned online_publications,offline_publications,subscriptions,gate_subscription_base;
static char injected_topic[512]="rx/command";
static bool inject_large,inject_subscription,fail_send,missing_ack,handler_nested;
static unsigned disconnects,opens,closes,cycles,ready_events,close_events;
static bool open_fail,subscribe_fail;
#ifdef REAL_SERVICE
static void real_handler(MessageData *);
static char last_topic[512],last_body[10000];
static unsigned busy_publications;static char busy_target[512];
#endif
#ifdef OVERLOAD_FIXTURE
static void overload_handler(MessageData *);
static void overload_publication(const unsigned char *, unsigned, const unsigned char *, size_t);
#endif
static Client client;
static Network network;
static mqtt_account_t account;
static mico_thread_t thread=1;
static void append(const void*p,size_t n){assert(rx_size+n<=sizeof(rx));memcpy(rx+rx_size,p,n);rx_size+=n;}
static void incoming(const char *topic,const char *body,unsigned id){
 unsigned char p[12000];MQTTString t=MQTTString_initializer;t.cstring=(char*)topic;
 int n=MQTTSerialize_publish(p,sizeof(p),0,1,0,id,t,(unsigned char*)body,strlen(body));assert(n>0);append(p,n);
}
static int read_packet(Network*n,unsigned char*p,int count,int timeout){
 (void)n;
 if(rx_at==rx_size){now_ms+=timeout>0?(unsigned)timeout:1;return 0;}
 assert(count>=0);size_t got=(size_t)count;if(got>rx_size-rx_at)got=rx_size-rx_at;
 memcpy(p,rx+rx_at,got);rx_at+=got;return (int)got;
}
static void sent_packet(void){
 unsigned length=0,mult=1;size_t offset=1;
 do{assert(offset<tx_size);length+=(tx[offset]&127)*mult;mult*=128;}while(tx[offset++]&128);
 assert(length+offset==tx_size);
 if(tx[0]>>4==1){unsigned char ack[]={0x20,2,0,0};append(ack,sizeof(ack));}
 if(tx[0]>>4==14)disconnects++;
 if(tx[0]>>4==12){unsigned char ack[]={0xd0,0};append(ack,sizeof(ack));}
 if(tx[0]>>4==8){
  subscriptions++;
  if(inject_subscription){incoming("rx/command","{}",91);inject_subscription=false;}
  unsigned char ack[]={0x90,3,tx[offset],tx[offset+1],subscribe_fail?0x80:1};append(ack,sizeof(ack));
 }
 if(tx[0]>>4==3){
  publications++;
  unsigned topic_size=(tx[offset]<<8)|tx[offset+1];
  size_t body_at=offset+2+topic_size+(((tx[0]>>1)&3)?2:0);
  if(topic_size==15&&!memcmp(tx+offset+2,"tx/availability",15)){
   assert((tx[0]&1)&&((tx[0]>>1)&3)==1);
   if(tx_size-body_at==6&&!memcmp(tx+body_at,"online",6)){
    online_publications++;
    if(gate_mode==7)gate_current=false;
    if(gate_mode==8)gate_cancel=true;
   }
   if(tx_size-body_at==7&&!memcmp(tx+body_at,"offline",7))offline_publications++;
  }
#ifdef REAL_SERVICE
  unsigned tn=(tx[offset]<<8)|tx[offset+1];assert(tn<sizeof(last_topic));
  memcpy(last_topic,tx+offset+2,tn);last_topic[tn]=0;
  size_t start=offset+2+tn+(((tx[0]>>1)&3)?2:0),len=tx_size-start;
  assert(len<sizeof(last_body));memcpy(last_body,tx+start,len);last_body[len]=0;
  if(strstr(last_body,"\"busy\"")){busy_publications++;strcpy(busy_target,last_topic);}
#endif
#ifdef OVERLOAD_FIXTURE
  overload_publication(tx+offset+2,topic_size,tx+body_at,tx_size-body_at);
#endif
  if(gate_mode>=5&&cycles==3&&topic_size==8&&!memcmp(tx+offset+2,"tx/state",8)){
   if(gate_mode==5)gate_current=false;
   if(gate_mode==6)gate_cancel=true;
  }
  if(inject_large){unsigned char huge[]={0x30,0xff,0xff,0x7f};append(huge,sizeof(huge));inject_large=false;}
  for(unsigned i=0;i<inject_count;i++)incoming(injected_topic,"{\"request_id\":\"paho-1\"}",77+i);
  inject_count=0;
  bool gate_ack_missing=gate_mode==2 && topic_size==15 &&
      !memcmp(tx+offset+2,"tx/availability",15) && tx_size-body_at==6 &&
      !memcmp(tx+body_at,"online",6);
  if(!missing_ack&&!gate_ack_missing){unsigned tn=(tx[offset]<<8)|tx[offset+1];size_t at=offset+2+tn;unsigned char ack[]={0x40,2,tx[at],tx[at+1]};append(ack,sizeof(ack));}
 }
 tx_size=0;
}
static int write_packet(Network*n,unsigned char*p,int count,int timeout){
 (void)n;(void)timeout;if(fail_send){now_ms++;return -1;}
 unsigned got=count;if(got>write_fragment)got=write_fragment;
 assert(tx_size+got<=sizeof(tx));memcpy(tx+tx_size,p,got);tx_size+=got;
 size_t off=1;unsigned length=0,mult=1;
 while(off<tx_size){unsigned c=tx[off++];length+=(c&127)*mult;if(!(c&128)){if(off+length==tx_size)sent_packet();break;}mult*=128;}
 return got;
}
static void network_close(Network *n){closes++;n->my_socket=-1;}
int NewNetwork(Network *n,char *host,int port,ssl_opts options){
 (void)host;(void)port;(void)options;opens++;n->mqttread=read_packet;n->mqttwrite=write_packet;n->disconnect=network_close;
 n->my_socket=open_fail?-1:3;return open_fail?-1:0;
}
static void handler(MessageData*d){
 assert(ph_mqtt_debug_core()->protocol_depth==0);
 assert(ph_mqtt_debug_core()->dispatch_depth==1);
#ifdef REAL_SERVICE
 real_handler(d);return;
#endif
#ifdef OVERLOAD_FIXTURE
 overload_handler(d);return;
#endif
 assert(!strcmp(d->topicName->cstring,"rx/command"));executions++;
 if(handler_nested){handler_nested=false;inject_count=1;MQTTMessage m={0};m.qos=1;m.payload="nested";m.payloadlen=6;
  assert(ph_mqtt_publish(&client,"tx/result",&m)==0);assert(executions==1);}
}
static bool accept(MessageData*d,messageHandler h){(void)d;assert(h==handler);return true;}
static void init(void){
 rx_at=rx_size=tx_size=0;network.mqttread=read_packet;network.mqttwrite=write_packet;
 memset(&client,0xa5,sizeof(client));assert(ph_mqtt_client_init(&client,&network,50)==0);
 client.isconnected=1;client.keepAliveInterval=30;client.heartbeat_retry_max=1;countdown(&client.ping_timer,30);
 assert(ph_mqtt_subscribe(&client,"rx/#",QOS1,handler)==0);
 assert(ph_mqtt_subscribe(&client,"rx/+",QOS1,handler)==0);
}
static int publish(void){MQTTMessage m={0};m.qos=1;m.payload="independent wire";m.payloadlen=strlen(m.payload);return ph_mqtt_publish(&client,"tx/state",&m);}
static void cleanup(void){assert(ph_mqtt_client_deinit(&client)==0);assert(!ph_mqtt_debug_core()->bytes&&!ph_mqtt_debug_core()->credit);assert(!native_live);}
static int prepare(void *ctx,ph_mqtt_connection *spec){
 (void)ctx;rx_at=rx_size=tx_size=0;
 if(multi_session){
  unsigned a=multi_attempt++;
  open_fail=a<6;subscribe_fail=a==6;missing_ack=a==7;inject_large=a==8;
 }
 *spec=(ph_mqtt_connection){&account,"broker","user","password","device","tx/availability",1883,50,NULL,"test","full"};return 1;
}
static bool subscription_plan(void *ctx,unsigned i,ph_mqtt_subscription *spec){
 (void)ctx;if(i)return false;*spec=(ph_mqtt_subscription){"rx/#",QOS1,handler};return true;
}
static int initial(void *ctx){(void)ctx;
 if(gate_mode)assert(ph_mqtt_enqueue_send("external/gate","queued",6,1,true)==0);
 inject_count=1;unsigned n=executions;int rc=publish();assert(executions==n);return rc==0?PH_MQTT_MORE:rc;}
static int cycle(void *ctx){(void)ctx;cycles++;
 if(gate_mode){
  if(cycles<=(gate_mode==9?40u:3u))assert(online_publications==gate_online_before&&ready_events==gate_ready_before);
  if(gate_mode>=9)now_ms+=1000;
  if(gate_mode==10&&cycles>=4&&cycles<45){
   assert(ready_events==gate_ready_before+1&&online_publications==gate_online_before+1);
   assert(subscriptions==gate_subscription_base+1);
  }
  unsigned before=executions;inject_count=1;int rc=publish();assert(executions==before);
  if(gate_mode==4&&cycles==2)return PH_MQTT_RECONFIGURE;
  return rc;
 }
 if(multi_session){
  multi_cycles++;
  if(multi_ready==1)return MQTT_FAILURE;
  /* Keep receiving commands during every telemetry PUBACK. Both business and
   * publication must progress, with no business callback recursion. */
  unsigned before=executions;inject_count=1;int rc=publish();assert(executions==before);
  if(multi_cycles>=25)multi_done=true;return rc;
 }
 return 0;
}
static void closing(void *ctx,bool connected,int rc){(void)ctx;(void)rc;assert(connected==(!open_fail));close_events++;if(gate_mode)gate_closed=true;}
static void event(void *ctx,unsigned e,int rc){(void)ctx;(void)rc;if(e==PH_MQTT_READY){ready_events++;if(multi_session)multi_ready++;}}
static bool stop(void *ctx){(void)ctx;if(gate_mode)return gate_cancel||gate_closed||cycles>=(gate_mode==10?46u:gate_mode==9?42u:4u)||(gate_mode==3&&cycles>=2);return multi_session?multi_done:cycles>=2 || close_events;}
static void sessions(void){
 ph_mqtt_session_hooks h={NULL,prepare,subscription_plan,initial,cycle,closing,event,stop};
 unsigned n=executions,online_before=online_publications,offline_before=offline_publications;
 ph_mqtt_session_run(&client,&h);assert(cycles==2&&ready_events==1&&close_events==1&&executions==n+1);
 assert(opens==1&&closes==1&&disconnects==1&&!native_live);
 assert(online_publications==online_before+1&&offline_publications==offline_before+1);
 cycles=close_events=0;open_fail=true;ph_mqtt_session_run(&client,&h);open_fail=false;
 assert(!cycles&&close_events==1&&opens==2&&closes==2&&!native_live);
 cycles=close_events=0;subscribe_fail=true;ph_mqtt_session_run(&client,&h);subscribe_fail=false;
 assert(!cycles&&close_events==1&&opens==3&&closes==3&&!native_live);
 multi_session=true;multi_attempt=multi_ready=multi_cycles=backoff_count=0;
 unsigned before_exec=executions,before_pub=publications,before_open=opens,before_close=closes;
 ph_mqtt_session_run(&client,&h);multi_session=false;
 const unsigned expected[]={1000,2000,4000,8000,15000,30000,30000,30000,30000,1000};
 assert(backoff_count==sizeof(expected)/sizeof(expected[0]));
 for(unsigned i=0;i<backoff_count;i++)assert(backoffs[i]==expected[i]);
 assert(multi_ready==2&&multi_cycles==25&&executions>=before_exec+24&&publications>=before_pub+24);
 assert(opens-before_open==11&&closes-before_close==11&&!native_live);
 ph_mqtt_diagnostics diagnostic;ph_mqtt_snapshot(&diagnostic);assert(diagnostic.errors&&diagnostic.reconnects>=10&&!diagnostic.protocol_depth);

}
static bool initial_gate(void *ctx){(void)ctx;return gate_mode==10?(cycles==3||cycles>=45):cycles>=(gate_mode==9?40u:3u);}
static bool gate_valid(void *ctx){(void)ctx;return gate_current;}
static void gated_sessions(void){
 ph_mqtt_session_hooks h={NULL,prepare,subscription_plan,initial,cycle,closing,event,stop,initial_gate,gate_valid};
 for(unsigned mode=1;mode<=10;mode++){
  gate_mode=mode;gate_closed=gate_cancel=false;gate_current=true;cycles=close_events=0;
  gate_online_before=online_publications;gate_ready_before=ready_events;
  unsigned ran=executions,published=publications,subscribed=subscriptions;gate_subscription_base=subscriptions;
  ph_mqtt_session_run(&client,&h);
  assert(gate_closed&&close_events==1&&!native_live&&!charged_live);
  assert(!ph_mqtt_debug_core()->bytes&&!ph_mqtt_debug_core()->credit);
  /* Paho callbacks enqueue only; commands and external SEND keep advancing
   * while the adapter is not ready, without publishing online prematurely. */
  assert(executions>=ran+2&&publications>=published+3);
  if(mode==1||mode>=9)assert(ready_events==gate_ready_before+1&&online_publications==gate_online_before+1);
  else assert(ready_events==gate_ready_before);
  if(mode>=3&&mode<=6)assert(online_publications==gate_online_before);
  if(mode==2||mode==7||mode==8)assert(online_publications==gate_online_before+1);
  if(mode==9)assert(subscriptions==subscribed+1);
  if(mode==10)assert(subscriptions==subscribed+1);
 }
 gate_mode=0;
}
int main(void){
 ph_mqtt_startup startup={0};uint32_t generation=ph_mqtt_startup_prepare(&startup);
 pending_startup=&startup;pending_generation=generation;
 assert(ph_mqtt_startup_wait(&startup,generation));
 generation=ph_mqtt_startup_prepare(&startup);uint32_t start=now_ms;
 assert(!ph_mqtt_startup_wait(&startup,generation)&&now_ms-start==1000);
 ph_mqtt_startup_publish(&startup,generation,false);assert(!ph_mqtt_startup_wait(&startup,generation));
 uint32_t old=generation;generation=ph_mqtt_startup_prepare(&startup);
 ph_mqtt_startup_publish(&startup,old,true);assert((__atomic_load_n(&startup.state,__ATOMIC_ACQUIRE)&3u)==0);
 ph_mqtt_startup_publish(&startup,generation,true);assert(!ph_mqtt_startup_wait(&startup,old));
 assert(ph_mqtt_startup_wait(&startup,generation));
 assert(mqtt_account_init(&account,1,"a1","test","aabbccddeeff"));
 assert(ph_mqtt_owner_init(&account,8192,&thread,accept));
 /* Actual pinned ClientInit's first and second buffer allocations fail.
  * The wrapper must restore Network.read and free the partially built Client. */
 for(unsigned point=1;point<=2;point++){
  memset(&client,0xa5,sizeof(client));network.mqttread=read_packet;
  fail_allocation=native_allocs+point;
  assert(ph_mqtt_client_init(&client,&network,50)!=MQTT_SUCCESS);
  assert(!native_live&&!charged_live&&network.mqttread==read_packet);
  fail_allocation=0;init();cleanup();
 }
 init();inject_count=1;write_fragment=2;assert(publish()==0);assert(executions==0&&ph_mqtt_debug_core()->count[0]==1);
 ph_mqtt_diagnostics before_init,after_init;ph_mqtt_snapshot(&before_init);
 assert(ph_mqtt_client_init(&client,&network,50)!=0);ph_mqtt_snapshot(&after_init);
 assert(before_init.bytes==after_init.bytes&&before_init.generation==after_init.generation&&after_init.queued[0]==1);
 handler_nested=true;assert(ph_mqtt_poll(&client)==0);assert(executions==1&&ph_mqtt_debug_core()->count[0]==1);
 assert(ph_mqtt_poll(&client)==0&&executions==2&&!ph_mqtt_debug_core()->count[0]);
 unsigned before=publications;current_thread=2;assert(publish()!=0);assert(ph_mqtt_yield(&client,1)!=0);assert(ph_mqtt_client_deinit(&client)!=0);assert(publications==before);
 assert(ph_mqtt_enqueue_send("external/topic","one",3,1,true)==0);current_thread=1;assert(ph_mqtt_poll(&client)==0&&publications==before+1);cleanup();
 init();inject_count=4;assert(publish()==0);assert(executions==2&&ph_mqtt_debug_core()->count[0]==4);for(unsigned i=0;i<4;i++)assert(ph_mqtt_poll(&client)==0);assert(executions==6);cleanup();
 init();inject_large=true;assert(publish()!=0);assert(executions==6);cleanup();
 init();missing_ack=true;assert(publish()!=0);missing_ack=false;cleanup();
 init();fail_send=true;assert(publish()!=0);fail_send=false;cleanup();
 sessions();gated_sessions();
 init();uint32_t stale=ph_mqtt_send_generation();assert(stale);cleanup();
 ph_mqtt_owner_end();thread=current_thread=2;assert(ph_mqtt_owner_init(&account,8192,&thread,accept));init();
 unsigned allocated_before=native_allocs;unsigned ran_before=executions;
 assert(ph_mqtt_enqueue_send_generation("old/session","stale",5,1,false,stale)!=0);
 assert(native_allocs==allocated_before&&executions==ran_before);
 uint32_t fresh=ph_mqtt_send_generation();assert(fresh&&fresh!=stale);
 assert(ph_mqtt_enqueue_send_generation("new/session","fresh",5,1,false,fresh)==0);
 assert(ph_mqtt_poll(&client)==0);
 /* A periodic subscription may dispatch an already-matching PUBLISH before
  * SUBACK. Its business handler must wait until Subscribe has returned. */
 unsigned before_subscribe=executions;inject_subscription=true;
 assert(ph_mqtt_subscribe(&client,"rx/#",QOS1,handler)==0);
 assert(executions==before_subscribe&&ph_mqtt_debug_core()->count[0]==1);
 assert(ph_mqtt_poll(&client)==0&&executions==before_subscribe+1);
 cleanup();
 assert(ph_mqtt_topic_matches("a/+","a/b",3));assert(ph_mqtt_topic_matches("a/#","a",1));assert(!ph_mqtt_topic_matches("a/+","a/b/c",5));
 printf("pinned Paho + actual shared owner: deferred/overlap/partial TX/nested ACK/timeout/oversize/wrong-thread recovery PASS (%u business calls)\n",executions);
}

/* Actual dispatcher, HTTP product and MQTT owner code; RTOS locks use pthreads. */
#include "mico.h"
#include "common/service.c"
#include "common/httpd/product.h"
#include "common/json_owned.h"
#include "common/mqtt_account.h"
#include "common/mqtt/owner.h"
#define MQTT_SUCCESS 0
#define MQTT_FAILURE -1
static int mqtt_resource_result;
int ph_mqtt_resource_ready(void){return mqtt_resource_result;}
mico_thread_t *ph_mqtt_owner_thread(void){static __thread mico_thread_t owner;owner=(uintptr_t)pthread_self();return &owner;}
/* PRODUCTION_MQTT_TELEMETRY */
#include <pthread.h>
uint32_t test_now;
static pthread_mutex_t barrier_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t barrier=PTHREAD_COND_INITIALIZER;
static bool pause_builder,builder_entered,builder_continue;
static unsigned builds,sends,allocation,fail_at;
static int snapshot_rc;
static mqtt_account_t account;
/* The guard itself remains production code. Fail one real JSON allocation. */
static int test_gate_permit(size_t n){return json_heap_permit(n) && (!fail_at || ++allocation!=fail_at);}
json_object *automation_command(const char*a,json_object*p,const char**c,bool*h)
{(void)a;(void)p;(void)c;*h=false;return NULL;}
json_object *service_product_capabilities(void){return json_object_new_object();}
json_object *service_product(const char*a,json_object*p,const char**code){
 (void)a;(void)p;*code="ok";builds++;
 json_object *o=json_object_new_object();
 json_build_object_add(o,"model",json_object_new_string("a1"));
 for(unsigned i=0;i<20;i++){char key[20];snprintf(key,sizeof(key),"field%u",i);json_build_object_add(o,key,json_object_new_string("production graph"));}
 pthread_mutex_lock(&barrier_lock);
 if(pause_builder){builder_entered=true;pthread_cond_broadcast(&barrier);while(!builder_continue)pthread_cond_wait(&barrier,&barrier_lock);}
 pthread_mutex_unlock(&barrier_lock);return o;
}
static void clean(void){
 service_memory_state_t m;service_memory_snapshot(&m);
 assert(!m.active&&!m.reserved&&!m.waiting&&!json_owner);
 json_heap_stats_t j;json_heap_snapshot(&j);assert(!j.live_blocks);
}
static int send_result(const char*topic,const char*body,bool retained){
 assert(strstr(topic,"/result")&&!retained&&body);sends++;return MQTT_SUCCESS;
}
static int puback_reentry(const char*topic,const char*body,bool retained){
 assert(strstr(topic,"/state")&&retained&&strstr(body,"field19"));sends++;
 /* No graph lease OR budget reservation may cross synchronous QoS1 publish. */
 service_memory_state_t m;service_memory_snapshot(&m);assert(!m.active&&!m.reserved&&!json_owner);
 unsigned token=service_json_claim();assert(token);service_json_release(token);
 /* A second HTTP producer may build while this independent wire is sent.
  * Paho callback deferral itself is exercised by the real protocol harness. */
 ph_http_product nested={0};ph_http_exchange exchange={0};
 assert(ph_http_claim(&nested,&exchange,0,false)==PH_HTTP_REPLY);
 ph_http_json(&nested,&exchange,service_query("status.get"));
 assert(exchange.response.status==200);ph_http_product_release(&nested);
 assert(sends==1);return -17;
}
static void *snapshot_thread(void*unused){(void)unused;snapshot_rc=service_mqtt_snapshot(puback_reentry,&account);return NULL;}
static int forbidden_sender(const char*t,const char*b,bool r){(void)t;(void)b;(void)r;abort();}
static void preflight_gate(void){
 unsigned before=builds;
 allocation=0;fail_at=1;mqtt_resource_result=PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED;
 for(unsigned i=0;i<50;i++)assert(service_mqtt_snapshot(forbidden_sender,&account)==PH_MQTT_MORE);
 assert(builds==before&&!allocation);clean();
 mqtt_resource_result=-17;
 assert(service_mqtt_snapshot(forbidden_sender,&account)==-17&&builds==before&&!allocation);clean();
 mqtt_resource_result=MQTT_SUCCESS;fail_at=0;
}
static int busy_sender(const char*t,const char*b,bool r){
 assert(strstr(t,"/result")&&!r&&strstr(b,"\"request_id\":\"blocked\"")&&strstr(b,"\"code\":\"busy\""));return 0;
}
static unsigned capabilities_sent;
static int capabilities_sender(const char*t,const char*b,bool retained){
 assert(strstr(t,"/capabilities")&&b&&retained&&!json_owner);capabilities_sent++;return capabilities_sent==1?-19:0;
}
static void deferred_capabilities(void){
 unsigned token=service_json_claim();assert(token);
 assert(service_mqtt_capabilities(forbidden_sender,&account)==PH_MQTT_MORE);
 assert(service_mqtt_capabilities_pending&&!capabilities_sent);
 service_json_release(token);test_now+=1000;
 assert(service_mqtt_snapshot(capabilities_sender,&account)==-19);
 assert(service_mqtt_capabilities_pending&&capabilities_sent==1);
 assert(service_mqtt_try_capabilities(capabilities_sender,&account)==0);
 assert(!service_mqtt_capabilities_pending&&capabilities_sent==2);clean();
}
static void competition(void){
 pthread_t thread;pause_builder=true;builder_entered=builder_continue=false;
 assert(!pthread_create(&thread,NULL,snapshot_thread,NULL));
 pthread_mutex_lock(&barrier_lock);while(!builder_entered)pthread_cond_wait(&barrier,&barrier_lock);pthread_mutex_unlock(&barrier_lock);
 ph_http_product product={0};ph_http_exchange exchange={0};
 assert(ph_http_claim(&product,&exchange,0,false)==PH_HTTP_WAIT);
 assert(product.waiting&&!product.claim&&!product.json_claim);
 pthread_mutex_lock(&barrier_lock);pause_builder=false;builder_continue=true;pthread_cond_broadcast(&barrier);pthread_mutex_unlock(&barrier_lock);
 assert(!pthread_join(thread,NULL));assert(snapshot_rc==-17);

 assert(ph_http_claim(&product,&exchange,0,false)==PH_HTTP_REPLY);
 assert(product.claim&&product.json_claim&&!product.waiting);
 unsigned previous=builds;
 assert(service_mqtt_snapshot(forbidden_sender,&account)==PH_MQTT_MORE&&builds==previous);
 ph_http_json(&product,&exchange,service_query("status.get"));
 assert(exchange.response.status==200&&product.wire&&!product.json_claim&&!json_owner);
 unsigned token=service_json_claim();assert(token);service_json_release(token);
 ph_http_product_release(&product);clean();
}
/* Real scheduler handles the two-second WAIT and body deadlines, releasing the
 * actual production gate on timeout. No host or DUT sockets are opened. */
static ph_http_product product;
static ph_http_action begin(ph_http_exchange*x){ph_http_product_init(&product,x);return ph_http_claim(&product,x,x->request->length,x->request->length!=0);}
static ph_http_action retry(ph_http_exchange*x){return ph_http_claim(&product,x,x->request->length,x->request->length!=0);}
static bool body(ph_http_exchange*x,const void*p,size_t n){(void)x;return ph_http_body_copy(&product,p,n);}
static void complete(ph_http_exchange*x){ph_http_json(&product,x,ph_http_body_json(&product,x));}
static void release(ph_http_exchange*x,bool sent){(void)x;(void)sent;ph_http_product_release(&product);}
static unsigned admission(void){return 3;}
static const ph_http_app app={begin,retry,body,complete,release,admission};
static const char*input;static size_t position;static bool accepted,closed;
static int open_fd(unsigned n){(void)n;return 0;}
static int limit(int fd,unsigned n,ph_http_diagnostics*d){(void)fd;d->limit=n;return 0;}
static int accept_fd(int fd){(void)fd;accepted=true;return 1;}
static int nonblock(int fd){(void)fd;return 0;}
static int wait_fd(uint64_t r,uint64_t w,uint64_t*rr,uint64_t*ww,unsigned ms){test_now+=ms;*rr=r & (accepted?(closed?0:2):1);*ww=w;return 1;}
static int read_fd(int fd,void*p,size_t n){(void)fd;size_t left=strlen(input)-position;if(!left)return -1;if(n>left)n=left;memcpy(p,input+position,n);position+=n;return n;}
static int send_fd(int fd,const void*p,size_t n){(void)fd;(void)p;return n;}
static int close_fd(int fd){if(fd==1)closed=true;return 0;}
static int error(void){return 0;}static bool retry_io(void){return true;}
static void yield_io(void){test_now++;}
static const ph_http_io io={.now=mico_rtos_get_time,.open=open_fd,.limit=limit,.accept=accept_fd,.nonblock=nonblock,.wait=wait_fd,.recv=read_fd,.send=send_fd,.close=close_fd,.error=error,.retry=retry_io,.yield=yield_io};
static void timeout_case(const char*request,bool blocked){
 input=request;position=0;accepted=closed=false;test_now=0;
 unsigned token=blocked?service_json_claim():0;
 ph_http_server server;assert(ph_http_server_open(&server,&io,&app,3));
 for(unsigned i=0;i<2000&&!closed;i++)ph_http_server_step(&server);
 assert(accepted&&closed&&test_now<20000);ph_http_server_close(&server);
 assert(json_owner==token);service_json_release(token);clean();
}
int main(void){
 service_init();assert(ready);assert(mqtt_account_init(&account,1,"a1","test","aabbccddeeff"));
 unsigned a=service_json_claim();assert(a&&!service_json_claim());service_json_release(a);
 unsigned b=service_json_claim();assert(b&&b!=a);service_json_release(a);assert(!service_json_claim());service_json_release(b);clean();
 deferred_capabilities();preflight_gate();competition();
 timeout_case("GET / HTTP/1.1\r\n\r\n",true);
 timeout_case("POST / HTTP/1.1\r\nContent-Length: 2\r\n\r\n",false);
 /* Builder/encode OOM cleans graph and lease; the next HTTP operation recovers. */
 for(unsigned index=1;index<=150;index++){
  ph_http_product p={0};ph_http_exchange x={0};assert(ph_http_claim(&p,&x,0,false)==PH_HTTP_REPLY);
  allocation=0;fail_at=index;
  ph_http_json(&p,&x,service_query("status.get"));fail_at=0;
  ph_http_product_release(&p);clean();
 }
 ph_http_product p={0};ph_http_exchange x={0};assert(ph_http_claim(&p,&x,0,false)==PH_HTTP_REPLY);
 ph_http_json(&p,&x,service_query("status.get"));assert(x.response.status==200);ph_http_product_release(&p);clean();
 puts("Actual service/HTTP/MQTT graph exclusion: concurrent producer, PUBACK reentry, token isolation, WAIT/body timeout, OOM recovery PASS");
}

#include "tests/stubs/status_wire.inc"

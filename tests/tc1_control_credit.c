/* Real HTTP core/product, shared service/receipts, and TC1 business adapter.
 * Only RTOS, Flash and GPIO are host seams. No DUT or network IO. */
#include <stdbool.h>
static void persist_observer(void);
#define TC1_PERSIST_OBSERVER() persist_observer()
#define main existing_tc1_business_main
#define start tc1_fixture_start
#define device_config_recovery tc1_fixture_recovery
#define device_config_recovery_set tc1_fixture_recovery_set
#include "tc1_business_mp.c"
#undef main
#undef start
#undef device_config_recovery
#undef device_config_recovery_set
#include "mico.h"
#include "common/httpd/product.h"
#include <errno.h>

uint32_t test_clock_ms=123000,test_heap_free=1024u*1024u;
size_t test_heap_live_bytes;
bool test_heap_dynamic=true;
bool device_config_recovery(void){return recovery;}
void device_config_recovery_set(bool value){recovery=value;}
static long allocation_budget=-1;
static unsigned allocations,frees,blocks;
static unsigned drop_after_allocation;
static size_t peak_bytes;
static pthread_mutex_t heap_lock=PTHREAD_MUTEX_INITIALIZER;
typedef union {struct {size_t bytes;unsigned magic;} value;long double align;} allocation;
void *__real_malloc(size_t);
void __real_free(void *);
void *__wrap_malloc(size_t bytes){
 pthread_mutex_lock(&heap_lock);
 if(!allocation_budget){pthread_mutex_unlock(&heap_lock);return NULL;}
 if(allocation_budget>0)allocation_budget--;
 allocation *p=__real_malloc(sizeof(*p)+bytes);
 if(p){p->value.bytes=bytes;p->value.magic=0x4e795ab1;allocations++;blocks++;test_heap_live_bytes+=bytes;if(test_heap_live_bytes>peak_bytes)peak_bytes=test_heap_live_bytes;if(drop_after_allocation==allocations)test_heap_free=SERVICE_MEMORY_RESERVE;}
 pthread_mutex_unlock(&heap_lock);return p?p+1:NULL;
}
void __wrap_free(void *pointer){
 if(!pointer)return;allocation *p=(allocation *)pointer-1;
 pthread_mutex_lock(&heap_lock);assert(p->value.magic==0x4e795ab1&&blocks);p->value.magic=0;
 assert(test_heap_live_bytes>=p->value.bytes);test_heap_live_bytes-=p->value.bytes;blocks--;frees++;
 pthread_mutex_unlock(&heap_lock);__real_free(p);
}
static bool drop_on_persist,auth_revoked;
static ph_auth_state auth;
static void persist_observer(void){
 if(drop_on_persist){test_heap_free=SERVICE_MEMORY_RESERVE;allocation_budget=0;}
}
bool ph_auth_context_valid(const ph_auth_context *c,unsigned scopes){
 return c&&c->owner==&auth&&!auth_revoked&&(c->scopes&scopes)==scopes;
}
static ph_auth_context admin(void){return (ph_auth_context){.owner=&auth,.source=PH_AUTH_HTTP_OPEN,.scopes=PH_AUTH_SCOPE_ADMIN};}
static void object(ph_writer *w){phw_begin(w,NULL,false);phw_end(w);}
void service_product_info_mp(ph_writer *w){object(w);}
void service_product_status_mp(ph_writer *w){object(w);}
void service_product_diagnostics_mp(ph_writer *w){object(w);}
void service_product_capabilities_mp(ph_writer *w,const char *const *extra,bool restricted){(void)extra;(void)restricted;object(w);}
bool automation_mp(const char *name,const ph_value *p,ph_writer *w,const char **code){(void)name;(void)p;(void)w;(void)code;return false;}
bool service_product_ota_start(const char *url){(void)url;return true;}

typedef struct {unsigned char bytes[256];size_t length;} command;
static command request(const char *action,const char *id,bool enabled,bool invalid){
 command q;ph_writer w;ph_buf b;assert(phw_init_fixed(&w,1,&b,q.bytes,sizeof(q.bytes)));
 phw_begin(&w,NULL,false);phw_text(&w,"action",action);if(id)phw_text(&w,"request_id",id);
 phw_begin(&w,"params",false);phw_bool(&w,!strcmp(action,"outlet.set")?"on":"enabled",enabled);
 if(invalid)phw_bool(&w,"unknown",true);phw_end(&w);phw_end(&w);q.length=phw_finish_fixed(&w);assert(q.length);return q;
}
static void code(const ph_http_exchange *x,const char *id,const char *expected){
 ph_value root,v;assert(x->response.body&&phv_parse(1,x->response.body,x->response.length,&root));
 assert(phv_get(&root,"code",&v)&&phv_equal_text(&v,expected));
 if(id)assert(phv_get(&root,"request_id",&v)&&phv_equal_text(&v,id));
}
static void reset_heap(void){allocation_budget=-1;test_heap_free=1024u*1024u;drop_on_persist=false;}
static void start(ph_http_product *p,ph_http_exchange *x,ph_http_request *r,const command *q){
 memset(r,0,sizeof(*r));r->length=q->length;x->request=r;
 ph_http_product_init(p,x);p->auth=admin();
}
static void feed(ph_http_product *p,const command *q){
 for(size_t i=0;i<q->length;i++)assert(ph_http_body_copy(p,q->bytes+i,1));
 assert(p->used==q->length&&!ph_http_body_copy(p,q->bytes,1));
}
static void finish(ph_http_product *p){
 ph_http_product_release(p);service_memory_state_t memory;service_memory_snapshot(&memory);
 assert(!memory.active&&!memory.reserved&&!memory.waiting);
}
static void run_control(command *q,const char *id,const char *expected,bool exhaust){
 ph_http_product p;ph_http_exchange x={0};ph_http_request r;
 reset_heap();start(&p,&x,&r,q);assert(ph_http_control_claim(&p,&x,q->length)==PH_HTTP_READ_BODY);feed(&p,q);
 unsigned before=allocations;
 if(exhaust){test_heap_free=SERVICE_MEMORY_RESERVE;allocation_budget=0;drop_on_persist=true;}
 assert(ph_http_control_command(&p,&x));code(&x,id,expected);assert(x.response.body==p.scalar);
 assert(allocations==before); /* Includes mutation, envelope and retained receipt. */
 unsigned char frozen[160];assert(x.response.length<=sizeof(frozen));memcpy(frozen,x.response.body,x.response.length);
 /* Body is still borrowed until completion; response remains owner-local and
  * independent through partial sends, with no dangling stack writer. */
 memset(p.body,0,p.used);assert(!memcmp(frozen,x.response.body,x.response.length));
 finish(&p);
}
static void test_faults(unsigned budget){
 command q=request("indicator.set","allocation-once",true,false);
 ph_http_product p;ph_http_exchange x={0};ph_http_request r;
 start(&p,&x,&r,&q);allocation_budget=budget;
 assert(ph_http_control_claim(&p,&x,q.length)==PH_HTTP_WAIT);
 assert(!p.body&&!p.control_credit&&!p.claim&&!blocks&&!writes);
 assert(service_control_begin());service_control_end(); /* WAIT holds no gate. */
 finish(&p);assert(!blocks);
 run_control(&q,"allocation-once","accepted",true);assert(writes==1&&config.power_led_enabled&&blocks==1);
 unsigned retained=blocks;run_control(&q,"allocation-once","accepted",true);assert(writes==1&&blocks==retained);
 q=request("indicator.set","allocation-once",false,false);run_control(&q,"allocation-once","conflict",true);assert(writes==1&&config.power_led_enabled&&blocks==retained);
}
static void test_guard_between_claim_and_credit(void){
 command q=request("indicator.set","guard-once",true,false);ph_http_product p;ph_http_exchange x={0};ph_http_request r;
 start(&p,&x,&r,&q);drop_after_allocation=1;
 assert(ph_http_control_claim(&p,&x,q.length)==PH_HTTP_WAIT);
 assert(allocations==1&&frees==1&&!p.body&&!p.control_credit&&!p.claim&&!writes);
 assert(service_control_begin());service_control_end();drop_after_allocation=0;reset_heap();
 /* Retry the SAME request/lease after pressure recedes. */
 assert(ph_http_control_claim(&p,&x,q.length)==PH_HTTP_READ_BODY);feed(&p,&q);
 allocation_budget=0;test_heap_free=SERVICE_MEMORY_RESERVE;
 assert(ph_http_control_command(&p,&x));code(&x,"guard-once","accepted");assert(writes==1);finish(&p);
}
static void test_dispatcher_interop(void){
 command q=request("indicator.set","http-first",true,false);run_control(&q,"http-first","accepted",true);
 reset_heap();ph_auth_context context=admin();ph_buf *wire=service_http_command_mp(q.bytes,q.length,&context);assert(wire);
 ph_http_exchange x={0};x.response.body=wire->buf;x.response.length=wire->len;code(&x,"http-first","accepted");ph_buf_free(wire);assert(writes==1);
 q=request("indicator.set","ordinary-first",false,false);wire=service_http_command_mp(q.bytes,q.length,&context);assert(wire);ph_buf_free(wire);assert(writes==2);
 run_control(&q,"ordinary-first","accepted",true);assert(writes==2);
 uint32_t revision=service_cache_revision();q=request("indicator.set",NULL,true,false);run_control(&q,NULL,"accepted",true);assert(writes==3&&service_cache_revision()!=revision);
 reset_heap();q=request("config.get","ordinary-read",false,false);ph_http_product p;ph_http_request r;
 start(&p,&x,&r,&q);assert(ph_http_control_claim(&p,&x,q.length)==PH_HTTP_READ_BODY);feed(&p,&q);
 assert(!ph_http_control_command(&p,&x)&&!p.control_credit);ph_http_command_mp(&p,&x,p.body,p.used);code(&x,"ordinary-read","invalid_params");assert(writes==3);finish(&p);
}
static void test_invalid_requests_do_not_execute(void){
 for(unsigned i=0;i<3;i++){
  char id[66];memset(id,'x',65);id[65]=0;
  command q=request("indicator.set",i==0?id:"invalid-wire",true,false);
  if(i==1)q.bytes[q.length++]=0; /* A trailing value is not one MP request. */
  if(i==2)q.length=0;
  ph_http_product p;ph_http_exchange x={0};ph_http_request r;start(&p,&x,&r,&q);
  assert(ph_http_control_claim(&p,&x,q.length)==PH_HTTP_READ_BODY);
  if(q.length)feed(&p,&q);
  assert(!ph_http_control_command(&p,&x)&&!p.control_credit);
  ph_http_command_mp(&p,&x,p.body,p.used);code(&x,NULL,"invalid_params");
  assert(!writes);finish(&p);assert(!blocks);
 }
}
static void header32(command *q,unsigned tag,unsigned n){
 q->bytes[q->length++]=(unsigned char)tag;
 for(int shift=24;shift>=0;shift-=8)q->bytes[q->length++]=(unsigned char)(n>>shift);
}
static void text32(command *q,const char *s){size_t n=strlen(s);header32(q,0xdb,(unsigned)n);memcpy(q->bytes+q->length,s,n);q->length+=n;}
static void test_longest_control_encodings(void){
 const char *actions[]={"indicator.set","child_lock.set","outlet.set"};size_t maximum=0;
 for(unsigned i=0;i<3;i++){
  command q={0};char id[65];memset(id,'x',64);id[0]='a'+i;id[64]=0;
  /* The longest MP representations of every map/string header and the
   * optional index still fit the materialized-body bound. */
  header32(&q,0xdf,3);text32(&q,"action");text32(&q,actions[i]);text32(&q,"request_id");text32(&q,id);text32(&q,"params");
  header32(&q,0xdf,i==2?2:1);
  if(i==2){text32(&q,"index");q.bytes[q.length++]=0xcf;for(unsigned j=0;j<7;j++)q.bytes[q.length++]=0;q.bytes[q.length++]=5;}
  text32(&q,i==2?"on":"enabled");q.bytes[q.length++]=0xc3;
  assert(q.length<=SERVICE_HTTP_CONTROL_INPUT);if(q.length>maximum)maximum=q.length;
  run_control(&q,id,"accepted",true);assert(writes==i+1);
 }
 printf("longest legal control encodings: maximum=%zu input_bound=%u PASS\n",maximum,SERVICE_HTTP_CONTROL_INPUT);
}
static void test_results(void){
 char id[65];memset(id,'x',64);id[64]=0;
 command q=request("indicator.set",id,true,false);fail_storage=true;run_control(&q,id,"storage_error",true);assert(writes==1&&!config.power_led_enabled);
 fail_storage=false;reset_heap();run_control(&q,id,"storage_error",true);assert(writes==1);
 q=request("indicator.set","reserved",true,false);deny_reserve=true;run_control(&q,"reserved","busy",true);assert(writes==1);deny_reserve=false;
 q=request("indicator.set","invalid",true,true);run_control(&q,"invalid","invalid_params",true);assert(writes==1);
 recovery=true;q=request("indicator.set","migration",true,false);run_control(&q,"migration","migration_required",true);assert(writes==1);recovery=false;
 auth_revoked=true;q=request("indicator.set","unauthorized",true,false);run_control(&q,"unauthorized","unauthorized",true);assert(writes==1);auth_revoked=false;
 q=request("child_lock.set","child",true,false);run_control(&q,"child","accepted",true);assert(childLockEnabled);
 q=request("outlet.set","outlet",true,false);run_control(&q,"outlet","accepted",true);assert(config.socket_status[0]&&config.socket_status[5]);
 /* Fixed reply capacity failures are handled before any product operation. */
 reset_heap();q=request("indicator.set","tiny",true,false);void *credit=service_http_control_credit();assert(credit);char bytes[16];size_t n;unsigned status;ph_auth_context context=admin();unsigned before=writes;
 assert(service_http_control_mp(q.bytes,q.length,&context,&credit,bytes,sizeof(bytes),&n,&status));assert(status==503&&writes==before&&credit);service_http_control_credit_free(credit);
}
static void test_retention(void){
 for(unsigned i=0;i<24;i++){
  char id[20];snprintf(id,sizeof(id),"bounded-%u",i);command q=request("indicator.set",id,i%2==0,false);run_control(&q,id,"accepted",true);
  assert(blocks==(i<8?i+1:8));
 }
 assert(blocks==8);printf("retained bytes=%zu blocks=%u peak=%zu\n",test_heap_live_bytes,blocks,peak_bytes);
}

static pthread_mutex_t gate_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_changed=PTHREAD_COND_INITIALIZER;
static bool waiting_at_gate;
static __thread bool http_worker;
int __real_sem_wait(sem_t *);
int __wrap_sem_wait(sem_t *s){
 if(http_worker){pthread_mutex_lock(&gate_lock);waiting_at_gate=true;pthread_cond_signal(&gate_changed);pthread_mutex_unlock(&gate_lock);}
 return __real_sem_wait(s);
}
struct pending {ph_http_product *p;ph_http_exchange *x;};
static void *dispatch_waiting(void *pointer){struct pending *v=pointer;http_worker=true;assert(ph_http_control_command(v->p,v->x));return NULL;}
static void test_revocation(void){
 command q=request("indicator.set","revoked-at-gate",true,false);ph_http_product p;ph_http_exchange x={0};ph_http_request r;
 start(&p,&x,&r,&q);assert(ph_http_control_claim(&p,&x,q.length)==PH_HTTP_READ_BODY);feed(&p,&q);
 assert(service_control_begin());pthread_t thread;struct pending v={&p,&x};assert(!pthread_create(&thread,NULL,dispatch_waiting,&v));
 pthread_mutex_lock(&gate_lock);while(!waiting_at_gate)pthread_cond_wait(&gate_changed,&gate_lock);pthread_mutex_unlock(&gate_lock);
 auth_revoked=true;service_control_end();assert(!pthread_join(thread,NULL));code(&x,"revoked-at-gate","unauthorized");assert(x.response.status==401&&!writes);finish(&p);assert(!blocks);
}

/* Run the real nonblocking HTTP core over deterministic byte IO. */
static ph_http_product product;
static command wire_command;
static bool accepted,peer_closed,allow_body,fail_send;
static unsigned send_limit=7;
static unsigned char input[700],output[1500];
static size_t input_length,input_at,output_length,header_length;
static int closes;
static ph_http_action begin(ph_http_exchange *x){ph_http_product_init(&product,x);product.auth=admin();return ph_http_control_claim(&product,x,x->request->length);}
static ph_http_action retry(ph_http_exchange *x){return ph_http_control_claim(&product,x,x->request->length);}
static bool body(ph_http_exchange *x,const void *bytes,size_t n){(void)x;return ph_http_body_copy(&product,bytes,n);}
static void complete(ph_http_exchange *x){assert(ph_http_control_command(&product,x));}
static void release(ph_http_exchange *x,bool sent){(void)x;(void)sent;finish(&product);}
static unsigned admission(void){return 3;}
static uint32_t now(void){return test_clock_ms;}
static int io_open(unsigned n){(void)n;return 3;}
static int limit(int fd,unsigned n,ph_http_diagnostics *d){(void)fd;(void)n;(void)d;return 0;}
static int accept_fd(int fd){(void)fd;if(accepted)return errno=EAGAIN,-1;accepted=true;return 4;}
static int nonblock(int fd){(void)fd;return 0;}
static int wait_io(uint64_t r,uint64_t w,uint64_t *rr,uint64_t *ww,unsigned ms){test_clock_ms+=ms;*rr=accepted?r&~(UINT64_C(1)<<3):r;*ww=w;return 1;}
static int recv_io(int fd,void *bytes,size_t n){
 (void)fd;if(input_at>=input_length)return peer_closed?0:(errno=EAGAIN,-1);
 if(!allow_body&&input_at>=header_length)return errno=EAGAIN,-1;
 if(n>3)n=3;if(n>input_length-input_at)n=input_length-input_at;
 memcpy(bytes,input+input_at,n);input_at+=n;return (int)n;
}
static int send_io(int fd,const void *bytes,size_t n){
 (void)fd;if(fail_send)return errno=EIO,-1;
 if(n>send_limit)n=send_limit;assert(output_length+n<=sizeof(output));memcpy(output+output_length,bytes,n);output_length+=n;return (int)n;
}
static int close_io(int fd){(void)fd;closes++;return 0;}
static bool again(void){return errno==EAGAIN;}
static int error_io(void){return errno;}
static void yield_io(void){}
static const ph_http_io io={now,io_open,limit,accept_fd,nonblock,wait_io,recv_io,send_io,close_io,again,error_io,yield_io,NULL};
static const ph_http_app app={begin,retry,body,complete,release,admission};
static void setup_wire(ph_http_server *server){
 wire_command=request("indicator.set","fragmented-once",true,false);
 int n=snprintf((char *)input,sizeof(input),"POST /api/v1/command HTTP/1.1\r\nContent-Length: %u\r\n\r\n",(unsigned)wire_command.length);assert(n>0);header_length=n;
 memcpy(input+n,wire_command.bytes,wire_command.length);input_length=n+wire_command.length;
 assert(ph_http_server_open(server,&io,&app,3));
}
static void test_core(unsigned scenario){
 ph_http_server server;setup_wire(&server);allow_body=false;
 if(scenario==0||scenario==4)allocation_budget=0;
 for(unsigned i=0;i<250 && server.slots[0].state!=PH_SLOT_BODY && server.slots[0].state!=PH_SLOT_WAIT;i++)ph_http_server_step(&server);
 assert(server.slots[0].state==((scenario==0||scenario==4)?PH_SLOT_WAIT:PH_SLOT_BODY));
 assert(!writes);
 if(scenario==0){
  assert(!product.control_credit&&!product.body);test_clock_ms+=1999;
  ph_http_server_step(&server); /* May already expire; deadline never renews. */
  test_clock_ms+=50;ph_http_server_step(&server);
 }else if(scenario==1){test_clock_ms+=16000;ph_http_server_step(&server);}
 else if(scenario==2){input_at=input_length;peer_closed=true;ph_http_server_step(&server);}
 else{
  if(scenario==4){reset_heap();for(unsigned i=0;i<20&&server.slots[0].state==PH_SLOT_WAIT;i++)ph_http_server_step(&server);assert(server.slots[0].state==PH_SLOT_BODY);}
  allow_body=true;drop_on_persist=true;
  for(unsigned i=0;i<500 && !writes;i++)ph_http_server_step(&server);
  assert(writes==1&&server.slots[0].state==PH_SLOT_SEND);
  assert(product.body&&product.control_credit==NULL&&product.claim); /* Lease outlives scalar sends. */
  if(scenario==5)fail_send=true;
 }
 for(unsigned i=0;i<500 && server.slots[0].state!=PH_SLOT_FREE;i++)ph_http_server_step(&server);
 if(scenario<3)assert(!writes&&!blocks);else{
  assert(writes==1&&blocks==1);
  if(scenario!=5){unsigned char *start=NULL;
   for(size_t i=0;i+4<output_length;i++)if(!memcmp(output+i,"\r\n\r\n",4)){start=output+i+4;break;}
   assert(start);ph_http_exchange x={0};x.response.body=start;x.response.length=output_length-(size_t)(start-output);code(&x,"fragmented-once","accepted");
  }
 }
 ph_http_server_close(&server);assert(!product.claim&&!product.body&&!product.control_credit);
 if(scenario==5){fail_send=false;run_control(&wire_command,"fragmented-once","accepted",true);assert(writes==1);}
}
int main(int argc,char **argv){
 assert(argc==2);service_init();unsigned scenario=(unsigned)strtoul(argv[1],NULL,10);
 if(scenario<2)test_faults(scenario);
 else if(scenario==2)test_results();
 else if(scenario==3)test_retention();
 else if(scenario==4)test_revocation();
 else if(scenario==9)test_dispatcher_interop();
 else if(scenario==10)test_core(4);
 else if(scenario==11)test_guard_between_claim_and_credit();
 else if(scenario==12)test_invalid_requests_do_not_execute();
 else if(scenario==13)test_core(5);
 else if(scenario==14)test_longest_control_encodings();
 else test_core(scenario-5);
 printf("PASS scenario=%u writes=%u allocations=%u frees=%u live=%u\n",scenario,writes,allocations,frees,blocks);return 0;
}

/* Product route/ownership callbacks are inserted byte-for-byte from production.
 * Only platform/config/flash/service boundaries below are replaced by stubs. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/httpd/product.h"
#include "common/http_cors.h"
#include "common/backup.h"
#include "common/typed_wire.h"
#include <limits.h>
#include "common/mqtt/diagnostics.h"
static bool maximum,command_alias,fail_diagnostics_alloc;
static unsigned wire_live,wire_allocations,fail_wire_at;
void *ph_wire_malloc(size_t n){wire_allocations++;if(fail_wire_at&&wire_allocations==fail_wire_at)return NULL;if(fail_diagnostics_alloc){fail_diagnostics_alloc=false;return NULL;}void *p=malloc(n);if(p)wire_live++;return p;}
void ph_wire_free(void *p){if(p){assert(wire_live);wire_live--;free(p);}}
void ph_mqtt_snapshot(ph_mqtt_diagnostics *d){memset(d,maximum?0xff:0,sizeof(*d));if(maximum){d->bytes=d->peak_bytes=d->credit=d->rx_buffer_bytes=d->tx_buffer_bytes=UINT32_MAX;d->last_error=INT32_MIN;d->failure.read.requested=d->failure.read.returned=d->failure.write.requested=d->failure.write.returned=INT32_MIN;}}
ph_http_diagnostics ph_http_snapshot(void){ph_http_diagnostics d;memset(&d,maximum?0xff:0,sizeof(d));if(maximum){d.last_close_rc=d.last_close_errno=d.last_select_rc=d.last_read_rc=d.limit=d.connections=d.connections_get_rc=d.read_errno=INT32_MIN;}return d;}
static unsigned claims,waiting,queries,commands,aborts,written,finishes,entered,reboots;
static bool deny,job,fail_write,fail_cap;
static unsigned ota_begins,activations;
static bool fail_activation,activation_uncertain,wire_fail_send,wire_fail_close,wire_server_running;
static char password[33];
/* The real envelope/storage transaction is covered in test_backup.py. Here the
 * device boundary exposes binary bytes so route ownership cannot assume text. */
static const unsigned char backup_bytes[128]={'P','H','C','B','A','C','K','1',0,0xff,0x80,0x21};
static unsigned backup_exports,backup_imports;
static bool fail_backup_export;
static const char *backup_import_code="accepted";
size_t device_backup_size(void){return sizeof(backup_bytes);}
bool device_backup_export(void *out,size_t n){
 assert(out&&n==sizeof(backup_bytes));backup_exports++;
 if(fail_backup_export)return false;
 memcpy(out,backup_bytes,n);return true;
}
const char *device_backup_import(const void *input,size_t n){
 assert(input&&n==sizeof(backup_bytes)&&!memcmp(input,backup_bytes,n));
 backup_imports++;return backup_import_code;
}
static uint32_t tick;
typedef struct {int free_memory,allocted_memory,num_of_chunks;} micoMemInfo_t;
static micoMemInfo_t memory={50000,1000,3};
static const micoMemInfo_t *MicoGetMemoryInfo(void){return &memory;}
static uint32_t mico_rtos_get_time(void){return tick;}
static void mico_thread_msleep(unsigned n){tick+=n;}
unsigned service_memory_claim(size_t n){if(deny)return 0;claims++;return service_memory_cost(n);}
void service_memory_release(unsigned c){assert(c&&claims);claims--;}
void service_memory_waiting(bool enter){if(enter)waiting++;else{assert(waiting);waiting--;}}
void service_memory_snapshot(service_memory_state_t*s){memset(s,maximum?0xff:0,sizeof(*s));}
void service_cache_changed(unsigned mask){assert(mask);}
ph_buf *service_query_mp(const char*a){assert(a);queries++;ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,NULL,false);phw_text(&w,"action",a);phw_end(&w);return phw_take(&w);}
ph_buf *service_command_mp(const void*p,size_t n){ph_value v;if(!phv_parse(PH_WIRE_MSGPACK,p,n,&v))return service_query_mp("invalid_request");commands++;ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,NULL,false);phw_text(&w,"action","command");phw_value(&w,"input",&v);phw_end(&w);return phw_take(&w);}
/* Auth core and token routes execute in test_auth_routes_mp. Here a revocable
 * bearer boundary isolates the real product routes/lifetimes/flash adapter. */
static bool token_valid=true;
ph_auth_state *device_local_auth(void){return (ph_auth_state *)password;}
bool ph_auth_password_is_set(ph_auth_state*p){return *(const char*)p!=0;}
static bool http_auth_authorized(const char*a){return token_valid&&!strcmp(a,"Bearer valid");}
static bool http_auth_route(const char*p){return !strcmp(p,"/api/v1/auth/login")||!strcmp(p,"/api/v1/auth/logout");}
static void http_auth_reply(ph_http_product*p,ph_http_exchange*x,unsigned status,const char*c,const char*t){(void)t;ph_http_message(p,x,status,c);}
static void http_auth_info(ph_http_product*p,ph_http_exchange*x){ph_http_message(p,x,200,"public_info");}
static void http_auth_complete(ph_http_product*p,ph_http_exchange*x){ph_http_message(p,x,200,"auth_test_boundary");}
static bool ota_job_active(void){return job;}
static void ota_job_poll(void){}
int ph_http_start(const ph_http_app*a,unsigned n){assert(a&&n==PH_HTTP_CLIENT_SLOTS);return 0;}

/* Real production scheduler below calls the inserted production adapter. */
static const char *wire_input;
static size_t wire_position,wire_size,wire_input_length;
static char wire_output[4096];
static bool wire_accepted,wire_closed,wire_stall;
static int wire_open(unsigned cap){(void)cap;return 0;}
static int wire_limit(int fd,unsigned cap,ph_http_diagnostics*d){(void)fd;d->limit=cap;return fail_cap&&cap==2?-1:0;}
static int wire_accept(int fd){assert(fd==0&&!wire_accepted);wire_accepted=true;return 1;}
static int wire_nonblock(int fd){(void)fd;return 0;}
static int wire_wait(uint64_t r,uint64_t w,uint64_t*rr,uint64_t*ww,unsigned ms){tick+=ms;*rr=*ww=0;if(!wire_accepted&&(r&1))*rr|=1;if(wire_accepted&&!wire_closed&&(r&2))*rr|=2;if(w&2)*ww|=2;return !!(*rr||*ww);}
static int wire_recv(int fd,void*p,size_t n){assert(fd==1);size_t left=wire_input_length-wire_position;if(!left&&wire_stall)return -1;if(n>left)n=left;memcpy(p,wire_input+wire_position,n);wire_position+=n;return n;}
static int wire_send(int fd,const void*p,size_t n){assert(fd==1);if(wire_fail_send)return -1;if(n>3)n=3;assert(wire_size+n<sizeof(wire_output));memcpy(wire_output+wire_size,p,n);wire_size+=n;return n;}
static int wire_close(int fd){if(fd==1){wire_closed=true;if(wire_fail_close)return -1;}return 0;}
static bool wire_retry(void){return wire_stall;}
static int wire_error(void){return 0;}
static void wire_yield(void){tick++;}
static const ph_http_io wire_io={mico_rtos_get_time,wire_open,wire_limit,wire_accept,wire_nonblock,wire_wait,wire_recv,wire_send,wire_close,wire_retry,wire_error,wire_yield,NULL};



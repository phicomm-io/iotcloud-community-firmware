/* Product route/ownership callbacks are inserted byte-for-byte from production.
 * Only platform/config/flash/service boundaries below are replaced by stubs. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/httpd/product.h"
#include "common/http_cors.h"
#include "json.h"
static bool maximum;
ph_http_diagnostics ph_http_snapshot(void){ph_http_diagnostics d;memset(&d,maximum?0xff:0,sizeof(d));if(maximum){d.last_close_rc=d.last_close_errno=d.last_select_rc=d.last_read_rc=d.limit=d.connections=d.connections_get_rc=d.read_errno=INT32_MIN;}return d;}
static unsigned claims,waiting,queries,commands,aborts,written,finishes,entered,reboots;
static bool deny,job,fail_write,fail_cap;
static unsigned ota_begins;
static char password[33];
static uint32_t tick;
typedef struct {int free_memory,allocted_memory,num_of_chunks;} micoMemInfo_t;
static micoMemInfo_t memory={50000,1000,3};
static const micoMemInfo_t *MicoGetMemoryInfo(void){return &memory;}
static uint32_t mico_rtos_get_time(void){return tick;}
static void mico_thread_msleep(unsigned n){tick+=n;}
unsigned service_memory_claim(size_t n){if(deny)return 0;claims++;return service_memory_cost(n);}
void service_memory_release(unsigned c){assert(c&&claims);claims--;}
void service_memory_waiting(bool enter){if(enter)waiting++;else{assert(waiting);waiting--;}}
uint64_t service_memory_failures(void){return 0;}
void service_memory_snapshot(service_memory_state_t*s){memset(s,maximum?0xff:0,sizeof(*s));}
json_object *service_query(const char*a){assert(a);queries++;json_object*o=json_object_new_object();json_build_object_add(o,"action",json_object_new_string(a));return o;}
json_object *service_command(json_object*r){commands++;return r?service_query("command"):NULL;}
json_object *service_parse(const void*p,size_t n){char*copy=malloc(n+1);memcpy(copy,p,n);copy[n]=0;json_object*o=json_tokener_parse(copy);free(copy);return o;}
static bool ota_job_active(void){return job;}
int ph_http_start(const ph_http_app*a,unsigned n){(void)a;(void)n;return 0;}

/* Real production scheduler below calls the inserted production adapter. */
static const char *wire_input;
static size_t wire_position,wire_size;
static char wire_output[4096];
static bool wire_accepted,wire_closed;
static int wire_open(unsigned cap){(void)cap;return 0;}
static int wire_limit(int fd,unsigned cap,ph_http_diagnostics*d){(void)fd;d->limit=cap;return fail_cap&&cap==2?-1:0;}
static int wire_accept(int fd){assert(fd==0&&!wire_accepted);wire_accepted=true;return 1;}
static int wire_nonblock(int fd){(void)fd;return 0;}
static int wire_wait(uint64_t r,uint64_t w,uint64_t*rr,uint64_t*ww,unsigned ms){tick+=ms;*rr=*ww=0;if(!wire_accepted&&(r&1))*rr|=1;if(wire_accepted&&!wire_closed&&(r&2))*rr|=2;if(w&2)*ww|=2;return !!(*rr||*ww);}
static int wire_recv(int fd,void*p,size_t n){assert(fd==1);size_t left=strlen(wire_input)-wire_position;if(n>left)n=left;memcpy(p,wire_input+wire_position,n);wire_position+=n;return n;}
static int wire_send(int fd,const void*p,size_t n){assert(fd==1);if(n>3)n=3;assert(wire_size+n<sizeof(wire_output));memcpy(wire_output+wire_size,p,n);wire_size+=n;return n;}
static int wire_close(int fd){if(fd==1)wire_closed=true;return 0;}
static bool wire_retry(void){return false;}
static int wire_error(void){return 0;}
static void wire_yield(void){tick++;}
static const ph_http_io wire_io={mico_rtos_get_time,wire_open,wire_limit,wire_accept,wire_nonblock,wire_wait,wire_recv,wire_send,wire_close,wire_retry,wire_error,wire_yield,NULL};

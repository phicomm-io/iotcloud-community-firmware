/* Product route/ownership callbacks are inserted byte-for-byte from production.
 * Only platform/config/flash/service boundaries below are replaced by stubs. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/httpd/product.h"
#include "common/http_cors.h"
#include "json.h"
static unsigned claims,waiting,queries,commands,aborts,written,finishes,entered,reboots;
static bool deny,job,fail_write;
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
void service_memory_snapshot(service_memory_state_t*s){memset(s,0,sizeof(*s));}
json_object *service_query(const char*a){assert(a);queries++;json_object*o=json_object_new_object();json_build_object_add(o,"action",json_object_new_string(a));return o;}
json_object *service_command(json_object*r){commands++;return r?service_query("command"):NULL;}
json_object *service_parse(const void*p,size_t n){char*copy=malloc(n+1);memcpy(copy,p,n);copy[n]=0;json_object*o=json_tokener_parse(copy);free(copy);return o;}
static bool ota_job_active(void){return job;}
int ph_http_start(const ph_http_app*a,unsigned n,bool nd){(void)a;(void)n;(void)nd;return 0;}

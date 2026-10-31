/* Host-only allocator ledger. Counts the entire old/new allocation overlap;
 * shrinking realloc deliberately retains storage, as on the pinned BK SDK. */
#include "port_internal.h"
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
void *__real_malloc(size_t);
void *__real_realloc(void *,size_t);
void __real_free(void *);
int __real_ph_mqtt_receipt_send_bytes(const mqtt_target_t *,const void *,size_t,void *,void (*)(void *));
static pthread_mutex_t ledger=PTHREAD_MUTEX_INITIALIZER;
static struct {void *pointer;size_t bytes,charge;} blocks[256];
static size_t live,peak,minimum=SIZE_MAX;
static unsigned generation,fail_min,fail_max,fail_left,failures,cache_allocations,cache_releases,reallocations;
static unsigned receipt_pressure,pressure_ms,pressure_at,pressure_left;
static size_t idle=8072;
/* Optional, separately reported ARM static-RAM delta for integrated LAN tests.
 * The original low-MQTT regression leaves this at zero. */
static size_t layout_credit;
static bool connected_budget;
/* The observed connected idle includes 2x512-byte Paho buffers and a 2 KiB
 * optional status cache. Keep the same total budget with/without that cache. */
static size_t charge(size_t n){return (n+31u)&~(size_t)15u;}
static void controls(void) {
 const char *path=getenv("PH_HOST_HEAP_CONTROL");if(!path)return;
 int fd=open(path,O_RDONLY);if(fd<0)return;char text[192];ssize_t n=read(fd,text,sizeof(text)-1);close(fd);if(n<=0)return;text[n]=0;
 unsigned version,low,lo,hi,count,pressure,ms;
 if(sscanf(text,"%u %u %u %u %u %u %u",&version,&low,&lo,&hi,&count,&pressure,&ms)!=7||version==generation)return;
 generation=version;idle=low;fail_min=lo;fail_max=hi;fail_left=count;receipt_pressure=pressure;pressure_ms=ms;
}
static size_t available(void) {
 if(bl_mqtt_status==MQTT_READY)connected_budget=true;
 /* Ambient low heap stays low across reconnect; releasing Paho/cache blocks
  * is what restores free bytes, not changing the logical connection state. */
 size_t budget=(connected_budget?idle+2u*charge(512)+charge(PH_STATUS_CACHE_MAX):13144u)+layout_credit;
 unsigned now=ph_os_now();size_t pressure=(uint32_t)(now-pressure_at)<pressure_ms?pressure_left:0;
 size_t used=live+pressure,result=budget>used?budget-used:0;
 if(result<minimum)minimum=result;return result;
}
size_t __wrap_xPortGetFreeHeapSize(void) {
 pthread_mutex_lock(&ledger);controls();size_t result=available();pthread_mutex_unlock(&ledger);return result;
}
void *__wrap_malloc(size_t n) {
 pthread_mutex_lock(&ledger);controls();size_t cost=charge(n);
 bool fail=(fail_left&&n>=fail_min&&n<=fail_max);
 if(fail)--fail_left;
 if(fail||cost>available()){++failures;pthread_mutex_unlock(&ledger);return NULL;}
 void *p=__real_malloc(n);if(p){unsigned i=0;while(i<256&&blocks[i].pointer)++i;assert(i<256);blocks[i].pointer=p;blocks[i].bytes=n;blocks[i].charge=cost;live+=cost;if(live>peak)peak=live;if(n==PH_STATUS_CACHE_MAX)++cache_allocations;}
 pthread_mutex_unlock(&ledger);return p;
}
void *__wrap_calloc(size_t count,size_t size) {
 if(size&&count>SIZE_MAX/size)return NULL;
 size_t n=count*size;void *p=__wrap_malloc(n);if(p)memset(p,0,n);return p;
}
void __wrap_free(void *p) {
 if(!p)return;
 pthread_mutex_lock(&ledger);
 for(unsigned i=0;i<256;++i)if(blocks[i].pointer==p){assert(live>=blocks[i].charge);live-=blocks[i].charge;if(blocks[i].bytes==PH_STATUS_CACHE_MAX)++cache_releases;blocks[i].pointer=NULL;break;}
 pthread_mutex_unlock(&ledger);__real_free(p);
}
void *__wrap_realloc(void *p,size_t n) {
 if(!p)return __wrap_malloc(n);
 pthread_mutex_lock(&ledger);++reallocations;size_t old=0;
 for(unsigned i=0;i<256;++i)if(blocks[i].pointer==p){old=blocks[i].bytes;break;}
 pthread_mutex_unlock(&ledger);
 if(old>=n)return p; /* BK shrink is a no-op. */
 assert(old);void *next=__wrap_malloc(n);if(next){memcpy(next,p,old);__wrap_free(p);}return next;
}
int __wrap_ph_mqtt_receipt_send_bytes(const mqtt_target_t *target,const void *wire,size_t n,void *owned,void (*release)(void *)) {
 pthread_mutex_lock(&ledger);controls();
 if(receipt_pressure){pressure_at=ph_os_now();pressure_left=receipt_pressure;receipt_pressure=0;}
 pthread_mutex_unlock(&ledger);
 return __real_ph_mqtt_receipt_send_bytes(target,wire,n,owned,release);
}
/* File I/O stays outside allocator critical sections on a small host thread. */
static void *snapshots(void *unused) {
 (void)unused;const char *path=getenv("PH_HOST_HEAP_STATS");
 for(;;){
  usleep(50000);pthread_mutex_lock(&ledger);size_t free_bytes=available();
  unsigned values[]={generation,failures,cache_allocations,cache_releases,reallocations,fail_left};
  size_t sizes[]={free_bytes,live,peak,minimum};pthread_mutex_unlock(&ledger);
  if(path){FILE *f=fopen(path,"w");if(f){fprintf(f,"%zu %zu %zu %zu %u %u %u %u %u %u\n",sizes[0],sizes[1],sizes[2],sizes[3],values[0],values[1],values[2],values[3],values[4],values[5]);fclose(f);}}
 }
 return NULL;
}
__attribute__((constructor)) static void start_snapshots(void) {
 const char *credit=getenv("PH_HOST_STATIC_HEAP_CREDIT");if(credit)layout_credit=(size_t)strtoul(credit,NULL,10);
 pthread_t thread;assert(!pthread_create(&thread,NULL,snapshots,NULL));pthread_detach(thread);
}

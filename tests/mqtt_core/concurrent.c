#include "core.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sched.h>
static ph_mqtt_core core;
static pthread_mutex_t queue_lock=PTHREAD_MUTEX_INITIALIZER,memory_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_t owner_thread;
static size_t live,peak;
static unsigned allocated,freed;
static bool stopping;
static pthread_mutex_t schedule_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t schedule_condition=PTHREAD_COND_INITIALIZER;
static bool schedule_reset,reset_locked,reader_entered;
static void lock(void*p){
 (void)p;
 assert(!pthread_mutex_lock(&schedule_lock));
 bool is_owner=pthread_equal(owner_thread,pthread_self());
 if(schedule_reset&&!is_owner){reader_entered=true;pthread_cond_broadcast(&schedule_condition);}
 pthread_mutex_unlock(&schedule_lock);
 assert(!pthread_mutex_lock(&queue_lock));
 assert(!pthread_mutex_lock(&schedule_lock));
 if(schedule_reset&&is_owner){
  reset_locked=true;pthread_cond_broadcast(&schedule_condition);
  while(!reader_entered)pthread_cond_wait(&schedule_condition,&schedule_lock);
 }
 pthread_mutex_unlock(&schedule_lock);
}
static void unlock(void*p){(void)p;assert(!pthread_mutex_unlock(&queue_lock));}
static bool owner(void*p){(void)p;return pthread_equal(owner_thread,pthread_self());}
static uint32_t now(void*p){(void)p;return 0;}
static bool permit(void*p,size_t n){(void)p;return n<=PH_MQTT_QUEUE_BYTES;}
static void *allocate(void*p,size_t n){
 (void)p;size_t charge=(n+31u)&~(size_t)15u;
 size_t *q=malloc(n+sizeof(size_t));assert(q);*q=charge;
 pthread_mutex_lock(&memory_lock);live+=charge;if(live>peak)peak=live;++allocated;
 assert(live<=PH_MQTT_QUEUE_BYTES);pthread_mutex_unlock(&memory_lock);return q+1;
}
static void release(void*p,void*q){
 (void)p;size_t *base=(size_t*)q-1;
 /* Force producers to run while detached allocations are still alive. */
 sched_yield();pthread_mutex_lock(&memory_lock);assert(live>=*base);live-=*base;++freed;
 pthread_mutex_unlock(&memory_lock);free(base);
}
static void *producer(void*p){
 unsigned kind=(uintptr_t)p?PH_MQTT_COMMAND:PH_MQTT_SEND;char payload[2900];memset(payload,'a',sizeof(payload));unsigned i=0;
 for(;;){pthread_mutex_lock(&memory_lock);bool done=stopping;pthread_mutex_unlock(&memory_lock);if(done)break;
  char topic[24];snprintf(topic,sizeof(topic),"producer/%u",i++%6);
  ph_mqtt_enqueue(&core,kind,topic,strlen(topic),payload,sizeof(payload),1,false);
  ph_mqtt_diagnostics d;ph_mqtt_core_snapshot(&core,&d);assert(d.bytes+d.credit<=PH_MQTT_QUEUE_BYTES);
  sched_yield();
 }return NULL;
}
static void *scheduled_reader(void*p){
 (void)p;pthread_mutex_lock(&schedule_lock);
 while(!reset_locked)pthread_cond_wait(&schedule_condition,&schedule_lock);
 pthread_mutex_unlock(&schedule_lock);
 ph_mqtt_diagnostics d;ph_mqtt_core_snapshot(&core,&d);
 assert(!d.bytes&&!d.credit);
 assert(!ph_mqtt_enqueue_generation(&core,PH_MQTT_SEND,"old",3,"old",3,1,false,0,1));
 return NULL;
}
int main(void){
 owner_thread=pthread_self();ph_mqtt_hooks hooks={NULL,lock,unlock,owner,now,permit,allocate,release};
 assert(ph_mqtt_core_init(&core,&hooks));
 /* Deterministically enter the published lock hook from another thread while
  * reset owns the mutex; that reader resumes against the new empty state. */
 schedule_reset=true;pthread_t reader;
 assert(!pthread_create(&reader,NULL,scheduled_reader,NULL));
 assert(ph_mqtt_core_reset(&core));
 assert(!pthread_join(reader,NULL));
 schedule_reset=false;ph_mqtt_core_accepting(&core,true);
 pthread_t producers[3];
 for(unsigned i=0;i<3;i++)assert(!pthread_create(&producers[i],NULL,producer,(void*)(uintptr_t)(i%2)));
 for(unsigned i=0;i<3000;i++){
  /* Producers and snapshot readers must be able to enter the lock throughout
   * owner rebind, including while the mutable fields are reset. */
  ph_mqtt_core_accepting(&core,false);
  assert(ph_mqtt_clear(&core));
  uint32_t old_generation=core.generation;
  assert(ph_mqtt_core_reset(&core));
  assert(core.generation!=old_generation);
  assert(core.hooks.lock==lock&&core.hooks.unlock==unlock);
  ph_mqtt_core_accepting(&core,true);
  sched_yield();
 }
 pthread_mutex_lock(&memory_lock);stopping=true;pthread_mutex_unlock(&memory_lock);
 for(unsigned i=0;i<3;i++)assert(!pthread_join(producers[i],NULL));
 assert(ph_mqtt_clear(&core));assert(!live&&allocated==freed);
 printf("actual common queue: 3 concurrent producers/snapshot readers and 3000 clears/rebinds; charged peak=%zu <= %u; all %u allocations released\n",peak,PH_MQTT_QUEUE_BYTES,allocated);
}

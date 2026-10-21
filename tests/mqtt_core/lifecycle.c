/* Execute the production client binding/startup with real competing threads. */
#include "mico.h"
#include "client.h"
#include "queue.h"
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static bool done;
static unsigned first_seen,iterations;
int mico_rtos_init_mutex(mico_mutex_t *m){*m=1;return 0;}
void mico_rtos_lock_mutex(mico_mutex_t *m){assert(*m==1);assert(!pthread_mutex_lock(&mutex));}
void mico_rtos_unlock_mutex(mico_mutex_t *m){assert(*m==1);assert(!pthread_mutex_unlock(&mutex));}
bool mico_rtos_is_current_thread(mico_thread_t *t){return *t==(uintptr_t)pthread_self();}
uint32_t mico_rtos_get_time(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return t.tv_sec*1000u+t.tv_nsec/1000000u;}
void mico_thread_msleep(unsigned ms){usleep(ms*1000u);}
const micoMemInfo_t *MicoGetMemoryInfo(void){static const micoMemInfo_t h={64000};return &h;}
char expired(Timer*t){return (int32_t)(mico_rtos_get_time()-t->end_time)>=0;}
void countdown_ms(Timer*t,unsigned n){t->end_time=mico_rtos_get_time()+n;}
void countdown(Timer*t,unsigned n){countdown_ms(t,n*1000);}
int left_ms(Timer*t){return expired(t)?0:(int)(t->end_time-mico_rtos_get_time());}
void InitTimer(Timer*t){memset(t,0,sizeof(*t));}
static int read_bytes(Network*n,unsigned char*p,int count,int timeout){(void)n;(void)p;(void)count;(void)timeout;return 0;}
static void *producer(void *argument){
 (void)argument;
 __atomic_add_fetch(&first_seen,1,__ATOMIC_RELEASE);
 while(!__atomic_load_n(&done,__ATOMIC_ACQUIRE)){
  ph_mqtt_diagnostics d;ph_mqtt_snapshot(&d);
  assert(d.bytes+d.credit<=PH_MQTT_QUEUE_BYTES);
  assert(!ph_mqtt_owner());
  uint32_t generation=ph_mqtt_send_generation();
  ph_mqtt_enqueue_send_generation("producer/state","value",5,1,true,generation);
  ph_mqtt_enqueue_send("producer/direct","value",5,1,true);
  sched_yield();
 }
 return NULL;
}
typedef struct {ph_mqtt_startup startup;uint32_t generation;mico_thread_t handle;bool success;} start_case;
static void *worker(void *argument){
 start_case *s=argument;
 bool ready=ph_mqtt_startup_wait(&s->startup,s->generation);
 assert(ready==s->success);
 if(ready)assert(s->handle==12345); /* Ordinary handle write is published. */
 return NULL;
}
static void *stale_creator(void *argument){
 start_case *s=argument;
 for(unsigned i=0;i<10000;i++){
  ph_mqtt_startup_publish(&s->startup,s->generation,true);
  sched_yield();
 }
 return NULL;
}
static void startup_cases(void){
 for(unsigned i=0;i<40;i++){
  start_case s={0};s.success=i%2;s.generation=ph_mqtt_startup_prepare(&s.startup);
  pthread_t thread;assert(!pthread_create(&thread,NULL,worker,&s));
  /* Force the entry to run before the RTOS output handle becomes available. */
  usleep(2000);s.handle=12345;
  ph_mqtt_startup_publish(&s.startup,s.generation,s.success);
  assert(!pthread_join(thread,NULL));
  uint32_t old=s.generation;s.generation=ph_mqtt_startup_prepare(&s.startup);
  ph_mqtt_startup_publish(&s.startup,old,true);
  assert((__atomic_load_n(&s.startup.state,__ATOMIC_ACQUIRE)&3u)==0);
  ph_mqtt_startup_publish(&s.startup,s.generation,false);
  assert(!ph_mqtt_startup_wait(&s.startup,s.generation));
 }
 /* A stale creator races the generation transition and the valid creator.
  * The single-state conditional CAS cannot publish across this transition. */
 for(unsigned i=0;i<20;i++){
  start_case s={0};s.generation=ph_mqtt_startup_prepare(&s.startup);
  pthread_t thread;assert(!pthread_create(&thread,NULL,stale_creator,&s));
  uint32_t fresh=ph_mqtt_startup_prepare(&s.startup);
  ph_mqtt_startup_publish(&s.startup,fresh,true);
  assert(!pthread_join(thread,NULL));
  assert(ph_mqtt_startup_wait(&s.startup,fresh));
 }

}
int main(void){
 startup_cases();
 pthread_t threads[3];
 for(unsigned i=0;i<3;i++)assert(!pthread_create(&threads[i],NULL,producer,NULL));
 while(__atomic_load_n(&first_seen,__ATOMIC_ACQUIRE)!=3)sched_yield();
 mqtt_account_t account={0};mico_thread_t owner=(uintptr_t)pthread_self();
 for(iterations=0;iterations<3000;iterations++){
  assert(ph_mqtt_owner_init(&account,8192,&owner,NULL));
  Client client;Network network={0};network.mqttread=read_bytes;
  assert(ph_mqtt_client_init(&client,&network,20)==MQTT_SUCCESS);
  sched_yield();
  assert(ph_mqtt_client_deinit(&client)==MQTT_SUCCESS);
  ph_mqtt_owner_end();assert(!ph_mqtt_owner());
 }
 __atomic_store_n(&done,true,__ATOMIC_RELEASE);
 for(unsigned i=0;i<3;i++)assert(!pthread_join(threads[i],NULL));
 ph_mqtt_diagnostics d;ph_mqtt_snapshot(&d);assert(!d.bytes&&!d.credit);
 puts("actual client: first publication + 3000 rebinds vs 3 snapshot/SEND/owner callers; creator/worker startup order and failed/stale tokens PASS");
}

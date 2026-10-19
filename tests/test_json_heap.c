/* Actual pinned JSON library: native allocation ledger + every failure point. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "json.h"
#include "printbuf.h"
#include "json_heap.h"
static struct {void *pointer;size_t bytes;} ledger[8192];
static size_t live_bytes, live_blocks, peak_bytes, attempts, fail_at, permits, deny_at;
static size_t capacity=1024*1024;
static unsigned locked;
static int threaded;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
void *__real_malloc(size_t);
void *__real_calloc(size_t,size_t);
void *__real_realloc(void *,size_t);
void __real_free(void *);
char *__real_strdup(const char *);
extern char *strndup(const char *,size_t);
static size_t index_of(void *p){for(size_t i=0;i<8192;i++)if(ledger[i].pointer==p)return i;assert(0);return 0;}
static void remove_pointer(void *p){size_t i=index_of(p);live_bytes-=ledger[i].bytes;live_blocks--;ledger[i].pointer=NULL;}
static void add_pointer(void *p,size_t n){if(!p)return;size_t i=index_of(NULL);ledger[i].pointer=p;ledger[i].bytes=n;live_bytes+=n;live_blocks++;if(live_bytes>peak_bytes)peak_bytes=live_bytes;}
static int fail(void){return ++attempts==fail_at;}
void *__wrap_malloc(size_t n){if(fail())return NULL;void *p=__real_malloc(n);add_pointer(p,n);return p;}
void *__wrap_calloc(size_t c,size_t n){if(fail())return NULL;assert(!n||c<=SIZE_MAX/n);void *p=__real_calloc(c,n);add_pointer(p,c*n);return p;}
void *__wrap_realloc(void *p,size_t n){if(fail())return NULL;size_t i=p?index_of(p):0;void *q=__real_realloc(p,n);if(q){if(p){live_bytes-=ledger[i].bytes;live_blocks--;ledger[i].pointer=NULL;}add_pointer(q,n);}return q;}
char *__wrap_strdup(const char *s){if(fail())return NULL;char *p=__real_strdup(s);add_pointer(p,strlen(s)+1);return p;}
void __wrap_free(void *p){if(p)remove_pointer(p);__real_free(p);}
void json_heap_lock(void){assert(!pthread_mutex_lock(&mutex));assert(!locked);locked=1;}
void json_heap_unlock(void){assert(locked);locked=0;assert(!pthread_mutex_unlock(&mutex));}
int json_heap_permit(size_t n){assert(locked);return ++permits!=deny_at && live_bytes<=capacity && n<=capacity-live_bytes;}
static void empty(void){json_heap_stats_t s;json_heap_snapshot(&s);assert(!s.live_blocks && !live_bytes && !live_blocks && !locked);}
static void scenario(void){
 const char *input="{\"request_id\":\"leak-1\",\"params\":{\"a\":[1,2,3,4,5,6,7,8],\"v\":123.456,\"text\":\"hello\\nworld\"},\"action\":\"status.get\"}";
 json_object *o=json_tokener_parse(input);
 if(o){struct printbuf *wire=json_object_to_json_string_ex(o);if(wire)printbuf_free(wire);json_object_put(o);}
 if(!threaded)empty();
}
static void *worker(void *unused){(void)unused;for(unsigned i=0;i<1000;i++)scenario();return NULL;}
int main(void){
 /* The exported SDK strndup serves URL callers using native free. */
 char *url=strndup("url-native",3);assert(url && !strcmp(url,"url"));
 json_heap_stats_t native;json_heap_snapshot(&native);assert(!native.live_blocks);free(url);empty();
 attempts=permits=0;scenario();size_t total=attempts,total_permits=permits;
 for(size_t i=1;i<=total;i++){attempts=permits=0;fail_at=i;scenario();}
 fail_at=0;
 for(size_t i=1;i<=total_permits;i++){attempts=permits=0;deny_at=i;scenario();}
 deny_at=0;
 for(size_t i=0;i<10000;i++)scenario();
 threaded=1;pthread_t workers[4];
 for(unsigned i=0;i<4;i++)assert(!pthread_create(&workers[i],NULL,worker,NULL));
 for(unsigned i=0;i<4;i++)assert(!pthread_join(workers[i],NULL));
 threaded=0;empty();
 /* Realloc failure retains old ownership, overflow never reaches calloc,
  * and shrinking/freeing stay possible under pressure. */
 void *p=json_heap_malloc(10);assert(p);capacity=live_bytes;
 assert(!json_heap_realloc(p,20));json_heap_free(p);empty();
 assert(!json_heap_calloc(SIZE_MAX,2));empty();
 capacity=1024*1024;p=json_heap_realloc(NULL,10);assert(p);assert(!json_heap_realloc(p,0));empty();
 json_heap_stats_t s;json_heap_snapshot(&s);
 assert(s.allocation_failures && s.guard_denied && s.peak_blocks);
 printf("JSON leak ledger: 10000 serial + 4000 concurrent cycles, %zu native failure points, %zu guard failure points; live=0; peak bytes=%zu, blocks=%u\n",total,total_permits,peak_bytes,s.peak_blocks);
}

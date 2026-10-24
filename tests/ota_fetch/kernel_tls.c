/* Host implementation of the MiCO kernel API, using real TLS. This verifies
 * the production HTTP adapter and its lifetime/timeout handling, not the
 * proprietary kernel's TLS internals or hardware heap/stack use. */
#include "mico.h"
#include <openssl/ssl.h>
#include <assert.h>
#include <stdatomic.h>
static atomic_int semaphores, threads, sessions;
static unsigned allocations, injected;
static int forced_error;
static bool fail(const char *name) {
    const char *fault=getenv("OTA_TEST_FAULT");
    return fault && !strcmp(fault,name) && !injected++;
}
bool ota_test_clean(void) {
    /* The guard posts done immediately before exiting its RTOS thread. */
    for(unsigned i=0;i<100 && atomic_load(&threads);i++)usleep(1000);
    return !atomic_load(&semaphores) && !atomic_load(&threads) && !atomic_load(&sessions);
}
int mico_rtos_init_semaphore(mico_semaphore_t *s,int maximum) {
    (void)maximum;
    if(fail(++allocations==1?"sem1":"sem2"))return -1;
    *s=malloc(sizeof(**s));if(!*s)return -1;
    if(sem_init(*s,0,0)){free(*s);*s=NULL;return -1;}
    atomic_fetch_add(&semaphores,1);return 0;
}
int mico_rtos_set_semaphore(mico_semaphore_t *s){return sem_post(*s);}
int mico_rtos_get_semaphore(mico_semaphore_t *s,uint32_t ms) {
    if(ms==MICO_WAIT_FOREVER){int rc;do{rc=sem_wait(*s);}while(rc&&errno==EINTR);return rc;}
    struct timespec at;clock_gettime(CLOCK_REALTIME,&at);at.tv_sec+=ms/1000;at.tv_nsec+=(ms%1000)*1000000;
    if(at.tv_nsec>=1000000000){at.tv_sec++;at.tv_nsec-=1000000000;}
    return sem_timedwait(*s,&at);
}
int mico_rtos_deinit_semaphore(mico_semaphore_t *s){int rc=sem_destroy(*s);free(*s);*s=NULL;atomic_fetch_sub(&semaphores,1);return rc;}
typedef struct {void (*fn)(mico_thread_arg_t);mico_thread_arg_t arg;} launch;
static void *run(void *p){launch l=*(launch *)p;free(p);l.fn(l.arg);return NULL;}
int mico_rtos_create_thread(void *thread,unsigned priority,const char *name,void (*fn)(mico_thread_arg_t),unsigned stack,mico_thread_arg_t arg) {
    (void)thread;(void)priority;(void)name;(void)stack;
    if(fail("thread"))return -1;
    launch *l=malloc(sizeof(*l));if(!l)return -1;*l=(launch){fn,arg};pthread_t id;
    atomic_fetch_add(&threads,1);
    if(pthread_create(&id,NULL,run,l)){free(l);atomic_fetch_sub(&threads,1);return -1;}pthread_detach(id);return 0;
}
void mico_rtos_delete_thread(void *thread){(void)thread;atomic_fetch_sub(&threads,1);pthread_exit(NULL);}
void ssl_set_client_version(unsigned version){assert(version==TLS_V1_2_MODE);}
mico_ssl_t ssl_connect_sni(int fd,int ca_len,char *ca,char *host,int *error) {
    assert(ca_len==0&&ca==NULL&&host&&*host);
    if(fail("tls_alloc"))return NULL;
    SSL_CTX *ctx=SSL_CTX_new(TLS_client_method());if(!ctx)return NULL;
    SSL_CTX_set_min_proto_version(ctx,TLS1_2_VERSION);SSL_CTX_set_max_proto_version(ctx,TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx,SSL_VERIFY_NONE,NULL);
    SSL *ssl=SSL_new(ctx);SSL_CTX_free(ctx);if(!ssl)return NULL;
    SSL_set_fd(ssl,fd);SSL_set_tlsext_host_name(ssl,host);
    int rc=SSL_connect(ssl);if(rc!=1){*error=SSL_get_error(ssl,rc);SSL_free(ssl);return NULL;}atomic_fetch_add(&sessions,1);return ssl;
}
void ssl_set_using_nonblock(mico_ssl_t ssl,int value){(void)ssl;assert(value==1);}
int ssl_send(mico_ssl_t ssl,void *p,size_t n){if(fail("want_write")){forced_error=3;return -1;}return SSL_write(ssl,p,n>17?17:n);}
int ssl_recv(mico_ssl_t ssl,void *p,size_t n){if(fail("want_read")){forced_error=2;return -1;}if(fail("read_wants_write")){forced_error=3;return -1;}return SSL_read(ssl,p,n>23?23:n);}
int ssl_pending(mico_ssl_t ssl){return SSL_pending(ssl);}
int ssl_get_error(mico_ssl_t ssl,int rc){if(forced_error){int error=forced_error;forced_error=0;return error;}return SSL_get_error(ssl,rc);}
int ssl_close(mico_ssl_t ssl){SSL_free(ssl);atomic_fetch_sub(&sessions,1);return 0;}

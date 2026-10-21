/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mico.h"
#include "http.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include "http_trace_hooks.h"
static ph_http_server server;
static ph_http_diagnostics cached;
static const ph_http_app *application;
static volatile unsigned lifecycle; /* 0=stopped, 1=starting/running, 2=stop requested */
static unsigned initial_cap;
static bool use_nodelay;
static int listener_fd=-1;
static uint64_t response_started;
static bool listener_ready;
static unsigned owner_state, restarts;
static int io_errno;
static uint32_t last_observed;
static unsigned irq_lock(void)
{
#ifdef PH_HTTP_HOST_TEST
    return 0;
#else
    unsigned mask;
    __asm volatile("mrs %0, primask\ncpsid i" : "=r"(mask) :: "memory");
    return mask;
#endif
}
static void irq_unlock(unsigned mask)
{
#ifdef PH_HTTP_HOST_TEST
    (void)mask;
#else
    __asm volatile("msr primask, %0" :: "r"(mask) : "memory");
#endif
}
static void publish(const ph_http_diagnostics *d)
{ unsigned occupied=0;for(unsigned i=0;i<PH_HTTP_CLIENT_SLOTS;++i)occupied+=server.slots[i].state!=PH_SLOT_FREE;
  unsigned mask=irq_lock();cached=*d;cached.occupied=occupied;cached.owner_state=owner_state;cached.restarts=restarts;irq_unlock(mask); }
ph_http_diagnostics ph_http_snapshot(void)
{ unsigned mask=irq_lock();ph_http_diagnostics d=cached;irq_unlock(mask);return d; }
static int open_listener(unsigned cap)
{
    (void)cap;
    int fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(fd<0)return -1;
    listener_fd=fd;listener_ready=false;
    if(fd>=PH_HTTP_FD_LIMIT)return fd;
    struct sockaddr_in a;memset(&a,0,sizeof(a));a.sin_family=AF_INET;
    a.sin_port=htons(80);a.sin_addr.s_addr=INADDR_ANY;
    if(bind(fd,(struct sockaddr *)&a,sizeof(a))<0 || listen(fd,0)<0)return fd;
    listener_ready=true;return fd;
}
static void observe(int fd,ph_http_diagnostics *d)
{
    int value=-1;socklen_t n=sizeof(value);
    d->connections_get_rc=getsockopt(fd,IPPROTO_TCP,TCP_CONN_NUM,&value,&n);
    if(n!=sizeof(value))d->connections_get_rc=-1;
    d->connections=value;
    d->connections_sample_ms=mico_rtos_get_time();
    last_observed=d->connections_sample_ms;
}
static int limit(int fd,unsigned cap,ph_http_diagnostics *d)
{
    if(!listener_ready) { d->set_rc=-1;d->set_errno=errno;return -1; }
    int value=(int)cap;
    d->set_rc=setsockopt(fd,IPPROTO_TCP,TCP_MAX_CONN_NUM,&value,sizeof(value));
    d->set_errno=d->set_rc<0?errno:0;
    socklen_t n=sizeof(value);value=-1;
    d->limit_get_rc=getsockopt(fd,IPPROTO_TCP,TCP_MAX_CONN_NUM,&value,&n);
    if(n!=sizeof(value))d->limit_get_rc=-1;
    d->limit=value;
    observe(fd,d);
    return d->set_rc<0 || d->limit_get_rc<0 || d->limit!=(int)cap?-1:0;
}
static int nonblock(int fd)
{
    /* mocIP fcntl always consumes the third vararg, including F_GETFL. */
    HTTP_TRACE(HTTP_TRACE_NONBLOCK_ENTER,fd,0);
    int flags=fcntl(fd,F_GETFL,0);
    if(flags<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0)return -1;
    int observed=fcntl(fd,F_GETFL,0);
    if(observed<0 || !(observed&O_NONBLOCK))return -1;
    HTTP_TRACE(HTTP_TRACE_NONBLOCK_RETURN,fd,observed);
    return 0;
}
static int wait_ready(uint64_t reads,uint64_t writes,uint64_t *rr,uint64_t *ww,unsigned ms)
{
    if(listener_fd>=0 && (uint32_t)(mico_rtos_get_time()-last_observed)>=1000u)observe(listener_fd,&server.diagnostics);
    if(!reads && !writes) { mico_thread_msleep(ms);*rr=*ww=0;return 0; }
    fd_set r,w;FD_ZERO(&r);FD_ZERO(&w);
    for(unsigned fd=0;fd<PH_HTTP_FD_LIMIT;++fd) {
        if(reads&(UINT64_C(1)<<fd))FD_SET(fd,&r);
        if(writes&(UINT64_C(1)<<fd))FD_SET(fd,&w);
    }
    struct timeval t={ms/1000,(ms%1000)*1000};
    HTTP_TRACE(HTTP_TRACE_CLIENT_SELECT_ENTER,listener_fd,(int)ms);
    int rc=select(PH_HTTP_FD_LIMIT,&r,&w,NULL,&t);
    io_errno=rc<0?errno:0;
    HTTP_TRACE(HTTP_TRACE_CLIENT_SELECT_RETURN,listener_fd,rc);
    *rr=*ww=0;
    if(rc>0)for(unsigned fd=0;fd<PH_HTTP_FD_LIMIT;++fd) {
        if(FD_ISSET(fd,&r))*rr|=UINT64_C(1)<<fd;
        if(FD_ISSET(fd,&w))*ww|=UINT64_C(1)<<fd;
    }
    return rc;
}
static int accept_one(int fd)
{ HTTP_TRACE(HTTP_TRACE_ACCEPT_ENTER,fd,0);int child=accept(fd,NULL,NULL);io_errno=child<0?errno:0;HTTP_TRACE(HTTP_TRACE_ACCEPT_RETURN,child,child<0?errno:0);return child; }
static int receive(int fd,void *p,size_t n) { HTTP_TRACE(HTTP_TRACE_RECV_ENTER,fd,(int)n);int rc=recv(fd,p,n,0);io_errno=rc<0?errno:0;HTTP_TRACE(HTTP_TRACE_RECV_RETURN,fd,rc);return rc; }
static int transmit(int fd,const void *p,size_t n)
{
    uint64_t bit=UINT64_C(1)<<fd;
    if(use_nodelay && !(response_started&bit)) {
        response_started|=bit;
        int one=1;setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
    }
    HTTP_TRACE(HTTP_TRACE_SEND_ENTER,fd,(int)n);
    int rc=send(fd,p,n,0);io_errno=rc<0?errno:0;HTTP_TRACE(HTTP_TRACE_SEND_RETURN,fd,rc);return rc;
}
static int close_one(int fd) { HTTP_TRACE(HTTP_TRACE_CLOSE_ENTER,fd,0);int rc=close(fd);io_errno=rc<0?errno:0;HTTP_TRACE(HTTP_TRACE_CLOSE_RETURN,fd,rc);if(fd>=0 && fd<PH_HTTP_FD_LIMIT)response_started&=~(UINT64_C(1)<<fd);if(fd==listener_fd)listener_fd=-1;return rc; }
static bool retry(void) { return io_errno==EINTR || io_errno==EAGAIN || io_errno==EWOULDBLOCK; }
static int error_number(void) { return io_errno; }
static void yield(void) { mico_thread_msleep(1); }
static uint32_t now(void) { return mico_rtos_get_time(); }
static const ph_http_io io={now,open_listener,limit,accept_one,nonblock,wait_ready,receive,transmit,close_one,retry,error_number,yield,publish};
static void owner(mico_thread_arg_t arg)
{
    (void)arg;
    owner_state=1;restarts=0;
    for(unsigned attempt=0;attempt<3 && lifecycle==1;++attempt) {
        if(ph_http_server_open(&server,&io,application,initial_cap)) {
            while(lifecycle==1 && server.running)ph_http_server_step(&server);
            ph_http_server_close(&server);
        }
        if(lifecycle!=1 || server.diagnostics.close_failures)break;
        if(attempt<2) { ++restarts;mico_thread_msleep(100); }
    }
    owner_state=server.diagnostics.close_failures?3:lifecycle==2?0:2;
    publish(&server.diagnostics);
    unsigned mask=irq_lock();lifecycle=0;irq_unlock(mask);
    mico_rtos_delete_thread(NULL);
}
int ph_http_start(const ph_http_app *app,unsigned cap,bool nodelay)
{
    unsigned mask=irq_lock();
    if(lifecycle || owner_state==3) { irq_unlock(mask);return -1; }
    application=app;initial_cap=cap;use_nodelay=nodelay;lifecycle=1;owner_state=1;
    cached.owner_state=1;cached.restarts=0;
    irq_unlock(mask);
    int rc=mico_rtos_create_thread(NULL,MICO_APPLICATION_PRIORITY,"http_core",owner,PH_HTTP_STACK_BYTES,0);
    if(rc!=kNoErr) { mask=irq_lock();lifecycle=0;owner_state=2;cached.owner_state=2;irq_unlock(mask); }
    return rc;
}
int ph_http_stop(void)
{
    unsigned mask=irq_lock();bool stopped=lifecycle==0;if(!stopped)lifecycle=2;irq_unlock(mask);
    return stopped?kNoErr:kInProgressErr;
}

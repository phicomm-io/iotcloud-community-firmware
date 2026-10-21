#define _POSIX_C_SOURCE 200809L
#include "http.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
static unsigned port,active_cap=3;
static uint64_t received,begun,released;
static char large[32768];
static uint32_t now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint32_t)((uint64_t)t.tv_sec*1000+t.tv_nsec/1000000);}
static int open_listener(unsigned cap){(void)cap;int fd=socket(AF_INET,SOCK_STREAM,0);struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);if(fd<0||bind(fd,(struct sockaddr*)&a,sizeof(a))||listen(fd,64))abort();socklen_t n=sizeof(a);getsockname(fd,(struct sockaddr*)&a,&n);port=ntohs(a.sin_port);return fd;}
static int limit(int fd,unsigned n,ph_http_diagnostics*d){(void)fd;d->limit=n;return 0;}
static int accept_one(int fd){return accept(fd,NULL,NULL);}
static int nonblock(int fd){return fcntl(fd,F_SETFL,fcntl(fd,F_GETFL,0)|O_NONBLOCK);}
static int wait_one(uint64_t reads,uint64_t writes,uint64_t*rr,uint64_t*ww,unsigned ms){fd_set r,w;FD_ZERO(&r);FD_ZERO(&w);for(int i=0;i<64;i++){if(reads&(UINT64_C(1)<<i))FD_SET(i,&r);if(writes&(UINT64_C(1)<<i))FD_SET(i,&w);}struct timeval t={0,ms*1000};int rc=select(64,&r,&w,NULL,&t);*rr=*ww=0;if(rc>0)for(int i=0;i<64;i++){if(FD_ISSET(i,&r))*rr|=UINT64_C(1)<<i;if(FD_ISSET(i,&w))*ww|=UINT64_C(1)<<i;}return rc;}
static int receive(int fd,void*p,size_t n){return recv(fd,p,n,0);}
static int transmit(int fd,const void*p,size_t n){return send(fd,p,n,MSG_NOSIGNAL);}
static bool retry(void){return errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK;}
static int get_error(void){return errno;}
static void yield_one(void){struct timespec t={0,1000000};nanosleep(&t,NULL);}
static ph_http_action begin(ph_http_exchange*x){++begun;x->response.status=200;x->response.type="text/plain";x->response.body="ok";x->response.length=2;received=0;if(!strcmp(x->request->method,"HEAD"))x->response.suppress_body=true;if(!strcmp(x->request->path,"/304"))x->response.status=304;if(!strcmp(x->request->path,"/large")){x->response.body=large;x->response.length=sizeof(large);}if(!strcmp(x->request->path,"/body"))return PH_HTTP_READ_BODY;return PH_HTTP_REPLY;}
static ph_http_action again(ph_http_exchange*x){(void)x;return PH_HTTP_WAIT;}
static bool body(ph_http_exchange*x,const void*p,size_t n){(void)x;(void)p;received+=n;return true;}
static void complete(ph_http_exchange*x){(void)x;}
static void release(ph_http_exchange*x,bool sent){(void)x;(void)sent;released++;}
static unsigned admission(void){return active_cap;}
static const ph_http_io io={now,open_listener,limit,accept_one,nonblock,wait_one,receive,transmit,close,retry,get_error,yield_one,NULL};
static const ph_http_app app={begin,again,body,complete,release,admission};
int main(int argc,char **argv)
{
    bool soak_mode=argc!=1;
    if(soak_mode) {
        if(argc!=3 || strcmp(argv[1],"--cap") ||
           (strcmp(argv[2],"3") && strcmp(argv[2],"4"))) {
            fprintf(stderr,"usage: %s [--cap 3|4]\n",argv[0]);
            return 2;
        }
        active_cap=(unsigned)strtoul(argv[2],NULL,10);
    }
    signal(SIGPIPE,SIG_IGN);memset(large,'a',sizeof(large));
    ph_http_server s;
    if(!ph_http_server_open(&s,&io,&app,active_cap))return 2;
    printf("%u\n",port);fflush(stdout);
    fcntl(0,F_SETFL,O_NONBLOCK);char c;
    while(s.running){if(read(0,&c,1)>0)break;ph_http_server_step(&s);}
    ph_http_server_close(&s);
    if(soak_mode) {
        unsigned closed=0,free_slots=0;
        for(unsigned i=0;i<PH_HTTP_CLOSE_REASONS;++i)closed+=s.diagnostics.closed[i];
        for(unsigned i=0;i<PH_HTTP_CLIENT_SLOTS;++i)
            if(s.slots[i].state==PH_SLOT_FREE && s.slots[i].fd==-1 && !s.slots[i].lease)
                ++free_slots;
        fprintf(stderr,"TCP_SOAK cap=%u accepted=%u rejected=%u busy_sent=%u busy_failed=%u "
                "closed=%u begun=%" PRIu64 " released=%" PRIu64
                " active=%d queued=%d free_slots=%u "
                "close_failures=%u\n",active_cap,s.diagnostics.accepted,
                s.diagnostics.rejected,s.diagnostics.busy_sent,s.diagnostics.busy_failed,
                closed,begun,released,s.active,s.queued,free_slots,s.diagnostics.close_failures);
    } else {
        fprintf(stderr,"released=%" PRIu64 " close_failures=%u\n",
                released,s.diagnostics.close_failures);
    }
    return s.diagnostics.close_failures?3:0;
}

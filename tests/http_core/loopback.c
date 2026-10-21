#define _POSIX_C_SOURCE 200809L
#include "http.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
static unsigned port,received,released;
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
static ph_http_action begin(ph_http_exchange*x){x->response.status=200;x->response.type="text/plain";x->response.body="ok";x->response.length=2;received=0;if(!strcmp(x->request->method,"HEAD"))x->response.suppress_body=true;if(!strcmp(x->request->path,"/304"))x->response.status=304;if(!strcmp(x->request->path,"/large")){x->response.body=large;x->response.length=sizeof(large);}if(!strcmp(x->request->path,"/body"))return PH_HTTP_READ_BODY;return PH_HTTP_REPLY;}
static ph_http_action again(ph_http_exchange*x){(void)x;return PH_HTTP_WAIT;}
static bool body(ph_http_exchange*x,const void*p,size_t n){(void)x;(void)p;received+=n;return true;}
static void complete(ph_http_exchange*x){(void)x;}
static void release(ph_http_exchange*x,bool sent){(void)x;(void)sent;released++;}
static unsigned admission(void){return 3;}
static const ph_http_io io={now,open_listener,limit,accept_one,nonblock,wait_one,receive,transmit,close,retry,get_error,yield_one,NULL};
static const ph_http_app app={begin,again,body,complete,release,admission};
int main(void){signal(SIGPIPE,SIG_IGN);memset(large,'a',sizeof(large));ph_http_server s;if(!ph_http_server_open(&s,&io,&app,3))return 2;printf("%u\n",port);fflush(stdout);fcntl(0,F_SETFL,O_NONBLOCK);char c;while(s.running){if(read(0,&c,1)>0)break;ph_http_server_step(&s);}ph_http_server_close(&s);fprintf(stderr,"released=%u close_failures=%u\n",released,s.diagnostics.close_failures);return s.diagnostics.close_failures?3:0;}

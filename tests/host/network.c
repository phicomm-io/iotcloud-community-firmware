#include "MQTTMiCO.h"
#include <time.h>
#include <errno.h>
uint64_t host_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
char expired(Timer *t){return host_now()>=t->end_time;}
void countdown_ms(Timer *t,unsigned n){t->end_time=host_now()+n;}
void countdown(Timer *t,unsigned n){countdown_ms(t,n*1000);}
int left_ms(Timer *t){uint64_t now=host_now();return now>=t->end_time?0:t->end_time-now;}
void InitTimer(Timer *t){t->end_time=0;}
static int net_read(Network *n,unsigned char *p,int len,int timeout){
    int got=0;
    while(got<len){fd_set f;FD_ZERO(&f);FD_SET(n->my_socket,&f);struct timeval t={timeout/1000,(timeout%1000)*1000};int rc=select(n->my_socket+1,&f,NULL,NULL,&t);if(rc<=0)return got?got:rc;rc=recv(n->my_socket,p+got,len-got,0);if(rc<=0)return -1;got+=rc;}
    return got;
}
static int net_write(Network *n,unsigned char *p,int len,int timeout){(void)timeout;return send(n->my_socket,p,len,MSG_NOSIGNAL);}
static void net_close(Network *n){if(n->my_socket>=0)close(n->my_socket);n->my_socket=-1;}
int NewNetwork(Network *n,char *host,int port,ssl_opts opts){
    (void)opts;struct addrinfo hints={0},*a=NULL;hints.ai_socktype=SOCK_STREAM;char service[8];snprintf(service,sizeof(service),"%d",port);
    if(getaddrinfo(host,service,&hints,&a))return -1;
    n->my_socket=socket(a->ai_family,a->ai_socktype,a->ai_protocol);int rc=connect(n->my_socket,a->ai_addr,a->ai_addrlen);freeaddrinfo(a);if(rc){net_close(n);return -1;}
    n->mqttread=net_read;n->mqttwrite=net_write;n->disconnect=net_close;return 0;
}

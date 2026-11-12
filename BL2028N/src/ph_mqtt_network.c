/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mqtt_network.h"
#include <errno.h>
#include <limits.h>
#ifdef PH_HOST_TEST
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/ioctl.h>
#else
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#endif
void InitTimer(Timer *t) { t->deadline=ph_os_now(); }
int left_ms(Timer *t) { int32_t left=(int32_t)(t->deadline-ph_os_now());return left>0?left:0; }
char expired(Timer *t) { return left_ms(t)==0; }
void countdown_ms(Timer *t,unsigned ms) { t->deadline=ph_os_now()+(ms>INT32_MAX?INT32_MAX:ms); }
void countdown(Timer *t,unsigned sec) { countdown_ms(t,sec>INT32_MAX/1000u?INT32_MAX:sec*1000u); }
static int transfer(Network *n,unsigned char *bytes,int length,int ms,bool writing) {
    if(!n||n->my_socket<0||length<0||ms<0)return -1;
    Timer timer;countdown_ms(&timer,(unsigned)ms);int done=0;
    while(done<length) {
        int remaining=left_ms(&timer);if(!remaining)break;
        fd_set fds;FD_ZERO(&fds);FD_SET(n->my_socket,&fds);
        struct timeval tv={remaining/1000,(remaining%1000)*1000};
        int rc=select(n->my_socket+1,writing?NULL:&fds,writing?&fds:NULL,NULL,&tv);
        if(rc<0&&errno==EINTR)continue;
        if(rc<0)return -1;if(!rc)break;
        rc=writing?send(n->my_socket,bytes+done,(size_t)(length-done),0):recv(n->my_socket,bytes+done,(size_t)(length-done),0);
        if(rc<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
        if(rc<=0)return -1;done+=rc;
    }
    return done;
}
static int read_bytes(Network *n,unsigned char *p,int len,int ms) { return transfer(n,p,len,ms,false); }
static int write_bytes(Network *n,unsigned char *p,int len,int ms) { return transfer(n,p,len,ms,true); }
static void disconnect(Network *n) { if(n->my_socket>=0)close(n->my_socket);n->my_socket=-1; }
int ph_network_open(Network *n,const char *host,unsigned port,ph_network_options options) {
    (void)options;n->my_socket=-1;n->disconnect=disconnect;
    n->mqttread=read_bytes;n->mqttwrite=write_bytes;
    if(!host||!*host||!port||port>65535)return -1;
    struct hostent *entry=gethostbyname(host);if(!entry||entry->h_length!=4)return -1;
    int fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(fd<0)return -1;
    struct timeval timeout={3,0};
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    struct sockaddr_in address;memset(&address,0,sizeof(address));address.sin_family=AF_INET;
    address.sin_port=htons((uint16_t)port);memcpy(&address.sin_addr,entry->h_addr_list[0],4);
    /* lwIP send/receive timeouts do not bound a blocking TCP connect. */
    unsigned long nonblocking=1;
    if(fd>=FD_SETSIZE||ioctl(fd,FIONBIO,&nonblocking)<0){close(fd);return -1;}
    if(connect(fd,(struct sockaddr*)&address,sizeof(address))<0) {
        if(errno!=EINPROGRESS&&errno!=EWOULDBLOCK){close(fd);return -1;}
        Timer timer;countdown_ms(&timer,3000);
        for(;;) {
            int remaining=left_ms(&timer);if(!remaining){close(fd);return -1;}
            fd_set writes;FD_ZERO(&writes);FD_SET(fd,&writes);
            struct timeval tv={remaining/1000,(remaining%1000)*1000};
            int rc=select(fd+1,NULL,&writes,NULL,&tv);
            if(rc<0&&errno==EINTR)continue;
            int error=0;socklen_t size=sizeof(error);
            if(rc<=0||!FD_ISSET(fd,&writes)||getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size)<0||error){close(fd);return -1;}
            break;
        }
    }
    n->my_socket=fd;return 0;
}

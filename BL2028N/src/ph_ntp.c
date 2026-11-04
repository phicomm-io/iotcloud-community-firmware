/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "port_internal.h"
#ifndef PH_HOST_TEST
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#endif
#include <string.h>
#include "ntp.h"
extern OSStatus BkRandomNumberRead(void *,int);
/* The existing LAN worker owns this one socket. A reply never blocks LAN
 * processing; connected UDP also filters unexpected source IPs and ports. */
static int ntp_fd=-1;
static uint32_t ntp_at,ntp_started;
static bool ntp_attempted;
static char ntp_host[64];
static unsigned char ntp_nonce[8];
static unsigned ntp_server;
static void ntp_close(void){if(ntp_fd>=0)close(ntp_fd);ntp_fd=-1;}
void ph_ntp_poll(const char *host,bool enabled){
    uint32_t ms=rtos_get_time();
    if(strcmp(host,ntp_host)){
        ntp_close();strcpy(ntp_host,host);ntp_attempted=false;ntp_server=0;
    }
    if(!enabled){ntp_close();return;}
    if(ntp_fd>=0){
        fd_set reads;FD_ZERO(&reads);FD_SET(ntp_fd,&reads);struct timeval delay={0,0};
        if(select(ntp_fd+1,&reads,NULL,NULL,&delay)>0){
            unsigned char packet[68];int n=recv(ntp_fd,packet,sizeof(packet),0);uint32_t utc;
            if(n>=0&&ph_ntp_reply(packet,(size_t)n,ntp_nonce,&utc)){
                bl_lock();ph_clock_sync(&bl_device,utc);bl_unlock();
                ntp_at=ms+3600000u;ntp_server=0;ntp_close();
            }
        }
        if(ntp_fd>=0&&(uint32_t)(ms-ntp_started)>=5000u)ntp_close();
        return;
    }
    if(ntp_attempted&&(int32_t)(ms-ntp_at)<0)return;
    ntp_attempted=true;ntp_at=ms+60000u;
    static const char *fallback[]={"ntp1.aliyun.com","ntp.tencent.com","pool.ntp.org"};
    const char *server=ntp_server?fallback[ntp_server-1]:ntp_host;ntp_server=(ntp_server+1)%4;
    struct hostent *resolved=gethostbyname(server);
    if(!resolved||resolved->h_addrtype!=AF_INET||resolved->h_length!=4||!resolved->h_addr_list[0])return;
    struct sockaddr_in address;memset(&address,0,sizeof(address));address.sin_family=AF_INET;address.sin_port=htons(123);
    memcpy(&address.sin_addr,resolved->h_addr_list[0],4);
    /* SDK random calls fill at most four bytes. */
    if(BkRandomNumberRead(ntp_nonce,4)||BkRandomNumberRead(ntp_nonce+4,4))return;
    unsigned char packet[48]={0};packet[0]=0x23;memcpy(packet+40,ntp_nonce,8);
    ntp_fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(ntp_fd<0)return;
    if(connect(ntp_fd,(struct sockaddr *)&address,sizeof(address))||send(ntp_fd,packet,sizeof(packet),0)!=(int)sizeof(packet)){ntp_close();return;}
    ntp_started=ms;
}

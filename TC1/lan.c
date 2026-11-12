/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lan.h"
#include "main.h"
#include "mico_system.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../common/lan_wire_v4.h"
static char tc1_lan_id[13];
#define LOCAL_ID tc1_lan_id
static int udp=-1;
static mico_mutex_t tc1_lan_lock;
static bool tc1_lan_lock_ready,tc1_lan_initialized,tc1_lan_running,tc1_lan_ready,tc1_lan_stop_requested;
static uint32_t tc1_lan_boot;
#include "../common/runtime_lan_business.inc"
extern bool feature_lan_pair(const ph_value *,ph_writer *,const char *);
static void tc1_lan_worker(mico_thread_arg_t arg)
{
    (void)arg;char *wire=ph_wire_malloc(LAN_V4_FRAME_MAX+1u);
    if(!wire)goto finish;
    udp=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(udp>=0){
        struct sockaddr_in address;memset(&address,0,sizeof(address));address.sin_family=AF_INET;
        address.sin_port=htons(LAN_V4_PORT);address.sin_addr.s_addr=INADDR_ANY;
        if(bind(udp,(struct sockaddr *)&address,sizeof(address))<0){close(udp);udp=-1;}
    }
    if(udp<0)goto finish;
    lan_business_start(tc1_lan_boot,"tc1",VERSION);
    mico_rtos_lock_mutex(&tc1_lan_lock);tc1_lan_ready=true;mico_rtos_unlock_mutex(&tc1_lan_lock);
    for(;;){
        mico_rtos_lock_mutex(&tc1_lan_lock);bool stop=tc1_lan_stop_requested;mico_rtos_unlock_mutex(&tc1_lan_lock);
        if(stop)break;
        lan_business_poll();fd_set fds;FD_ZERO(&fds);FD_SET(udp,&fds);struct timeval wait={0,100000};
        if(select(udp+1,&fds,NULL,NULL,&wait)<=0)continue;
        struct sockaddr_in from;socklen_t length=sizeof(from);
        int n=recvfrom(udp,wire,LAN_V4_FRAME_MAX+1,0,(struct sockaddr *)&from,&length);
        if(n<=0||n>(int)LAN_V4_FRAME_MAX)continue;
        char ip[16];snprintf(ip,sizeof(ip),"%s",inet_ntoa(from.sin_addr));
        ph_value request;if(!phv_parse(PH_WIRE_MSGPACK,wire,(size_t)n,&request))continue;
        if(lan_business_input(&request,ip,ntohs(from.sin_port),wire,LAN_V4_FRAME_MAX))continue;
        /* Pair borrows the input. Encode into separate bounded storage because
         * the nonce/key are consumed while the reply is built. */
        ph_value op;if(!phv_get(&request,"op",&op)||(!phv_equal_text(&op,"auth_pair")&&!phv_equal_text(&op,"auth_resume")&&!phv_equal_text(&op,"auth_unpair")))continue;
        char response[256];ph_writer w;ph_buf b;
        if(phw_init_fixed(&w,PH_WIRE_MSGPACK,&b,response,sizeof(response))&&feature_lan_pair(&request,&w,ip)){
            size_t bytes=phw_finish_fixed(&w);if(bytes)sendto(udp,response,bytes,0,(struct sockaddr *)&from,length);
        }
        memset(response,0,sizeof(response));
    }
finish:
    lan_business_stop();if(udp>=0){close(udp);udp=-1;}ph_wire_free(wire);
    mico_rtos_lock_mutex(&tc1_lan_lock);tc1_lan_ready=tc1_lan_running=false;mico_rtos_unlock_mutex(&tc1_lan_lock);
    mico_rtos_delete_thread(NULL);
}
OSStatus tc1_lan_start(void)
{
    if(!tc1_lan_initialized){
        if(!tc1_lan_lock_ready)tc1_lan_lock_ready=mico_rtos_init_mutex(&tc1_lan_lock)==kNoErr;
        if(!tc1_lan_lock_ready)return kNoMemoryErr;
        if(MicoRandomNumberRead((uint8_t *)&tc1_lan_boot,sizeof(tc1_lan_boot))!=kNoErr)return kUnknownErr;
        if(!tc1_lan_boot)tc1_lan_boot=1;
        for(unsigned i=0;i<12;i++)tc1_lan_id[i]=(char)tolower((unsigned char)str_mac[i]);tc1_lan_id[12]=0;
        tc1_lan_initialized=true;
    }
    mico_rtos_lock_mutex(&tc1_lan_lock);
    if(tc1_lan_running){bool ready=tc1_lan_ready&&!tc1_lan_stop_requested;mico_rtos_unlock_mutex(&tc1_lan_lock);return ready?kNoErr:kInProgressErr;}
    tc1_lan_running=true;tc1_lan_ready=tc1_lan_stop_requested=false;mico_rtos_unlock_mutex(&tc1_lan_lock);
    OSStatus e=mico_rtos_create_thread(NULL,MICO_APPLICATION_PRIORITY+1,"lan_business",tc1_lan_worker,TC1_LAN_STACK_SIZE,0);
    if(e!=kNoErr){mico_rtos_lock_mutex(&tc1_lan_lock);tc1_lan_running=false;mico_rtos_unlock_mutex(&tc1_lan_lock);}return e==kNoErr?kInProgressErr:e;
}
bool tc1_lan_pause(unsigned timeout_ms)
{
    if(!tc1_lan_initialized)return true;
    mico_rtos_lock_mutex(&tc1_lan_lock);tc1_lan_stop_requested=true;mico_rtos_unlock_mutex(&tc1_lan_lock);
    uint32_t at=mico_rtos_get_time();
    for(;;){mico_rtos_lock_mutex(&tc1_lan_lock);bool running=tc1_lan_running;mico_rtos_unlock_mutex(&tc1_lan_lock);if(!running)return true;
        if((uint32_t)(mico_rtos_get_time()-at)>=timeout_ms)return false;mico_thread_msleep(20);}
}

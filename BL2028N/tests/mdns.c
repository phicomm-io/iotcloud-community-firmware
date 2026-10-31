/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mdns_sdk_stub.h"
#include "../src/ph_mdns.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct netif station={{0x01020304},true,true};
static struct mdns_service service;
static uint32_t now;
static size_t heap=32768;
static unsigned fault,queued,checked,adds,removes,services;
static bool tcpip,attached;
static void (*pending_callback)(void *);
static void *pending_arg;
uint32_t rtos_get_time(void){return now;}
size_t xPortGetFreeHeapSize(void){return heap;}
void *net_get_sta_handle(void){assert(tcpip);return &station;}
err_t tcpip_callback(void (*fn)(void *),void *arg)
{
    assert(!tcpip);if(fault==5)return ERR_MEM;
    assert(!pending_callback);pending_callback=fn;pending_arg=arg;queued++;return ERR_OK;
}
err_t mdns_resp_init_checked(void)
{assert(tcpip);checked++;return fault==1?ERR_MEM:fault==2?ERR_VAL:ERR_OK;}
err_t mdns_resp_add_netif(struct netif *netif,const char *host,uint32_t ttl)
{
    assert(tcpip&&netif==&station&&!attached&&ttl==30);assert(!strcmp(host,"iotcloud-s30-v1b-c12345"));adds++;
    if(fault==3)return ERR_MEM;attached=true;return ERR_OK;
}
err_t mdns_resp_remove_netif(struct netif *netif)
{assert(tcpip&&netif==&station&&attached);attached=false;removes++;return ERR_OK;}
s8_t mdns_resp_add_service(struct netif *netif,const char *name,const char *type,enum mdns_sd_proto proto,
                         uint16_t port,uint32_t ttl,service_get_txt_fn_t fn,void *arg)
{
    assert(tcpip&&attached&&netif==&station&&!strcmp(name,"iotcloud-s30-v1b-c12345"));
    assert(!strcmp(type,"_openiot")&&proto==DNSSD_PROTO_UDP&&port==11288&&ttl==30);services++;
    if(fault==4)return -1;memset(&service,0,sizeof(service));fn(&service,arg);return 0;
}
err_t mdns_resp_add_service_txtitem(struct mdns_service *s,const char *text,u8_t length)
{
    assert(tcpip&&s==&service&&length&&s->length+length+1<=sizeof(s->txt));
    memcpy(s->txt+s->length,text,length);s->length+=length;s->txt[s->length++]='\n';return ERR_OK;
}
static void drain(void)
{assert(pending_callback);void (*fn)(void *)=pending_callback;void *arg=pending_arg;pending_callback=NULL;tcpip=true;fn(arg);tcpip=false;}
static void poll_after(unsigned delay,bool ready)
{now+=delay;PH_BL_MdnsPoll(ready);drain();}
int main(void)
{
    assert(!PH_BL_MdnsInit("iotcloud-s30-v1b-000000","02aabb000000","full","v1.2.3-abcdef"));
    assert(!PH_BL_MdnsInit("openbeken","02aabbc12345","full","v1.2.3-abcdef"));
    assert(!PH_BL_MdnsInit("iotcloud-s30-v1b-c12345","03aabbc12345","full","v1.2.3-abcdef"));
    char host[32]="iotcloud-s30-v1b-c12345",id[13]="02aabbc12345",version[64]="v1.2.3-abcdef-dirty";
    assert(PH_BL_MdnsInit(host,id,"full",version));memset(host,'x',strlen(host));memset(id,'x',strlen(id));memset(version,'x',strlen(version));
    assert(!PH_BL_MdnsInit("iotcloud-s30-v1b-c12345","02aabbc12345","full","v1.2.3"));
    char txt[80];assert(PH_BL_MdnsTxt(0,txt,sizeof(txt))&&!strcmp(txt,"id=02aabbc12345"));
    assert(!PH_BL_MdnsTxt(0,txt,2));assert(!PH_BL_MdnsTxt(8,txt,sizeof(txt)));
    fault=1;PH_BL_MdnsPoll(true);for(unsigned i=0;i<100;i++)PH_BL_MdnsPoll(true);assert(queued==1);drain();assert(!PH_BL_MdnsActive());
    for(unsigned mode=2;mode<=4;mode++){fault=mode;poll_after(5001,true);assert(!PH_BL_MdnsActive());}
    fault=0;poll_after(5001,true);assert(PH_BL_MdnsActive()&&attached);service.txt[service.length]=0;
    const char *fields[]={"id=02aabbc12345\n","model=s30-v1b\n","variant=full\n","firmware=v1.2.3-abcdef-dirty\n","lan_wire=4\n","codec=msgpack\n","version=4\n","udp_port=11288\n"};
    for(unsigned i=0;i<8;i++)assert(strstr(service.txt,fields[i]));assert(service.length<256);assert(PH_BL_MdnsFailures()==4);
    poll_after(1001,false);assert(!PH_BL_MdnsActive()&&!attached);unsigned init_before=checked;
    heap=7680;poll_after(1,true);assert(!PH_BL_MdnsActive()&&checked==init_before);
    heap=32768;poll_after(5001,true);assert(PH_BL_MdnsActive());station.ip.addr++;
    unsigned old_removes=removes;poll_after(1001,true);assert(PH_BL_MdnsActive()&&removes==old_removes+1);
    station.link=false;poll_after(1001,true);assert(!PH_BL_MdnsActive());station.link=true;
    fault=5;now+=5001;PH_BL_MdnsPoll(true);assert(!pending_callback&&!PH_BL_MdnsActive());fault=0;
    poll_after(5001,true);assert(PH_BL_MdnsActive());poll_after(1001,false);assert(!PH_BL_MdnsActive()&&!attached);
    printf("mDNS shared SDK port: serialized callbacks/owned TXT/factory identity/4 init failures/low heap/IP-change/stop PASS (adds=%u services=%u removes=%u)\n",adds,services,removes);
    return 0;
}

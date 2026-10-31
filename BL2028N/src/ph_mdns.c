/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifdef PH_S30_LAN_V4
#include "ph_mdns.h"
#include "factory_mac.h"
#include "memory_policy.h"
#include "os.h"
#include <stdio.h>
#include <string.h>
static struct {char host[32],id[13],variant[33],firmware[64];} identity;
static bool identity_ready;
static uint32_t active,failures;
static bool ascii_label(const char *s,size_t max)
{
    if(!s||!*s||strlen(s)>max)return false;
    for(const unsigned char *p=(const unsigned char *)s;*p;p++)if(*p<33||*p>126)return false;
    return true;
}
bool PH_BL_MdnsInit(const char *host,const char *id,const char *variant,const char *firmware)
{
    if(identity_ready||!host||!id||strlen(id)!=12||!ascii_label(variant,32)||!ascii_label(firmware,63))return false;
    unsigned char mac[6];char expected[32];
    for(unsigned i=0;i<12;i++){
        char c=id[i];unsigned n=c>='0'&&c<='9'?(unsigned)(c-'0'):c>='a'&&c<='f'?(unsigned)(c-'a'+10):16;
        if(n>15)return false;if(!(i&1))mac[i/2]=(unsigned char)(n<<4);else mac[i/2]|=(unsigned char)n;
    }
    if(!ph_mac_host_name(expected,mac)||strcmp(expected,host))return false;
    strcpy(identity.host,host);strcpy(identity.id,id);strcpy(identity.variant,variant);strcpy(identity.firmware,firmware);
    identity_ready=true;return true;
}
size_t PH_BL_MdnsTxt(unsigned index,char *output,size_t capacity)
{
    if(!identity_ready||!output||!capacity)return 0;
    static const char *const keys[]={"id","model","variant","firmware","lan_wire","codec","version","udp_port"};
    const char *const values[]={identity.id,"s30-v1b",identity.variant,identity.firmware,"4","msgpack","4","11288"};
    if(index>=sizeof(keys)/sizeof(*keys))return 0;
    int n=snprintf(output,capacity,"%s=%s",keys[index],values[index]);
    return n>0&&(size_t)n<capacity&&(size_t)n<=255?(size_t)n:0;
}
bool PH_BL_MdnsActive(void){return ph_os_load(&active)!=0;}
unsigned PH_BL_MdnsFailures(void){return (unsigned)ph_os_load(&failures);}

#if !defined(PH_HOST_TEST) || defined(PH_MDNS_TEST)
#ifdef PH_MDNS_TEST
#include "../tests/mdns_sdk_stub.h"
#else
#include "net.h"
#include "lwip/apps/mdns.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
/* Added by the pinned mdns-init-checked.patch. Legacy void API is unchanged. */
extern err_t mdns_resp_init_checked(void);
#endif
static uint32_t pending,desired,retry_ms=1000;
static struct netif *published;
static uint32_t published_ip;
static uint32_t last_poll;
static bool previous_ready,polled;
static void record_failure(void)
{ph_os_store(&failures,ph_os_load(&failures)+1u);ph_os_store(&retry_ms,5000u);}
static void txt_callback(struct mdns_service *service,void *unused)
{
    (void)unused;char text[80];
    for(unsigned i=0;i<8;i++){
        size_t n=PH_BL_MdnsTxt(i,text,sizeof(text));
        if(!n||mdns_resp_add_service_txtitem(service,text,(u8_t)n)!=ERR_OK){record_failure();return;}
    }
}
static void update_in_tcpip(void *unused)
{
    (void)unused;struct netif *netif=(struct netif *)net_get_sta_handle();
    bool ready=ph_os_load(&desired)&&netif&&netif_is_up(netif)&&netif_is_link_up(netif)&&!ip4_addr_isany_val(*netif_ip4_addr(netif));
    uint32_t ip=ready?ip4_addr_get_u32(netif_ip4_addr(netif)):0;
    if(published&&(!ready||published!=netif||published_ip!=ip)){
        (void)mdns_resp_remove_netif(published);published=NULL;published_ip=0;ph_os_store(&active,0u);
    }
    if(ready&&!published){
        /* Includes host/service blocks and callback/pbuf headroom. Service
         * allocation still fails safely if a pool/block is unavailable. */
        if(ph_os_free_heap()<PH_SERVICE_MEMORY_RESERVE+1024u||mdns_resp_init_checked()!=ERR_OK){record_failure();goto done;}
        if(mdns_resp_add_netif(netif,identity.host,30)!=ERR_OK){record_failure();goto done;}
        if(mdns_resp_add_service(netif,identity.host,"_openiot",DNSSD_PROTO_UDP,11288,30,txt_callback,NULL)<0){
            (void)mdns_resp_remove_netif(netif);record_failure();goto done;
        }
        published=netif;published_ip=ip;ph_os_store(&active,1u);
    }
    ph_os_store(&retry_ms,1000u);
done:
    ph_os_store(&pending,0u);
}
void PH_BL_MdnsPoll(bool ready)
{
    if(!identity_ready)return;
    ph_os_store(&desired,ready?1u:0u);uint32_t now=ph_os_now();
    bool changed=previous_ready!=ready;previous_ready=ready;
    if(!ready&&!PH_BL_MdnsActive()&&!ph_os_load(&pending))return;
    if(!changed&&polled&&(uint32_t)(now-last_poll)<ph_os_load(&retry_ms))return;
    uint32_t expected=0;ph_os_cas32(&pending,&expected,1u);if(expected)return;
    last_poll=now;polled=true;
    if(tcpip_callback(update_in_tcpip,NULL)!=ERR_OK){ph_os_store(&pending,0u);record_failure();}
}
#else
/* Host socket services do not impersonate the SDK's multicast responder. */
void PH_BL_MdnsPoll(bool ready){(void)ready;}
#endif
#endif

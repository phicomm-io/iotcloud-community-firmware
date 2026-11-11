/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ph_link.h"
#include "os.h"

#if !defined(PH_HOST_TEST) || defined(PH_LINK_TEST)
#ifdef PH_LINK_TEST
#include "../tests/link_sdk_stub.h"
#else
#include "net.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/tcpip.h"
#endif

/* Silence is normal. First solicit a gateway ARP reply, which is unicast and
 * does not depend on ICMP firewall rules, DNS or an Internet service. A quiet
 * but working station answers before the next poll. Broadcast RX alone must
 * not hide the associated-but-unicast-deaf state observed on S30/BK7231N. */
#define LINK_IDLE_MS 30000u
#define LINK_PROBE_MS 5000u
#define LINK_PROBE_COUNT 3u
#define LINK_COOLDOWN_MS 300000u

static uint32_t unicast_rx,group_rx,input_errors,probes,probe_errors,recoveries;
static uint32_t desired,pending,requested,requested_rx,last_recovery,rearm;
/* Only tcpip_thread owns these fields. */
static struct netif *watched;
static uint32_t watched_ip,watched_gateway,seen_rx,activity_at,probe_at;
static unsigned unanswered;
/* Only the LAN owner writes these scheduling fields. */
static uint32_t last_poll;
static bool polled,previous_ready;

void PH_BL_LinkRx(void *interface,bool group,bool accepted)
{
    if(interface!=net_get_sta_handle())return;
    if(!accepted)ph_os_store(&input_errors,ph_os_load(&input_errors)+1u);
    else if(group)ph_os_store(&group_rx,ph_os_load(&group_rx)+1u);
    else ph_os_store(&unicast_rx,ph_os_load(&unicast_rx)+1u);
}

ph_link_stats PH_BL_LinkSnapshot(void)
{
    ph_link_stats s={ph_os_load(&unicast_rx),ph_os_load(&group_rx),ph_os_load(&input_errors),
        ph_os_load(&probes),ph_os_load(&probe_errors),ph_os_load(&recoveries)};
    return s;
}

static void update_in_tcpip(void *unused)
{
    (void)unused;
    struct netif *netif=net_get_sta_handle();uint32_t now=ph_os_now();
    bool ready=ph_os_load(&desired)&&netif&&netif_is_up(netif)&&netif_is_link_up(netif)&&
        !ip4_addr_isany_val(*netif_ip4_addr(netif))&&!ip4_addr_isany_val(*netif_ip4_gw(netif));
    uint32_t rx=ph_os_load(&unicast_rx);
    uint32_t reset=1;ph_os_cas32(&rearm,&reset,0u);if(reset==1u)watched=NULL;
    if(!ready){watched=NULL;unanswered=0;ph_os_store(&requested,0u);goto done;}
    uint32_t ip=ip4_addr_get_u32(netif_ip4_addr(netif)),gw=ip4_addr_get_u32(netif_ip4_gw(netif));
    if(watched!=netif||watched_ip!=ip||watched_gateway!=gw||seen_rx!=rx){
        watched=netif;watched_ip=ip;watched_gateway=gw;seen_rx=rx;
        activity_at=now;unanswered=0;ph_os_store(&requested,0u);
    }
    if(ph_os_load(&requested))goto done;
    if(ph_os_load(&recoveries)&&(uint32_t)(now-ph_os_load(&last_recovery))<LINK_COOLDOWN_MS){
        activity_at=now;unanswered=0;goto done;
    }
    if((uint32_t)(now-activity_at)<LINK_IDLE_MS)goto done;
    if(unanswered&&(uint32_t)(now-probe_at)<LINK_PROBE_MS)goto done;
    if(unanswered==LINK_PROBE_COUNT){
        ph_os_store(&requested_rx,rx);ph_os_store(&requested,1u);goto done;
    }
    /* Allocation/queue failure is local pressure, not proof of a deaf link.
     * Start a new complete sequence after it clears, with a bounded retry. */
    if(etharp_request(netif,netif_ip4_gw(netif))==ERR_OK){
        ph_os_store(&probes,ph_os_load(&probes)+1u);unanswered++;
    }else{
        ph_os_store(&probe_errors,ph_os_load(&probe_errors)+1u);
        unanswered=0;activity_at=now-(LINK_IDLE_MS-LINK_PROBE_MS);
    }
    probe_at=now;
done:
    ph_os_store(&pending,0u);
}

void PH_BL_LinkPoll(bool ready)
{
    ph_os_store(&desired,ready?1u:0u);
    if(!ready){ph_os_store(&requested,0u);ph_os_store(&rearm,1u);}
    uint32_t now=ph_os_now();bool changed=previous_ready!=ready;previous_ready=ready;
    if(!changed&&polled&&(uint32_t)(now-last_poll)<1000u)return;
    uint32_t expected=0;ph_os_cas32(&pending,&expected,1u);if(expected)return;
    last_poll=now;polled=true;
    /* A full TCP/IP mailbox must not block the LAN owner under RX pressure. */
    if(tcpip_try_callback(update_in_tcpip,NULL)!=ERR_OK)ph_os_store(&pending,0u);
}

bool PH_BL_LinkTakeRecovery(bool allowed)
{
    uint32_t now=ph_os_now();
    if(!allowed||!ph_os_load(&desired)||ph_os_load(&requested_rx)!=ph_os_load(&unicast_rx)||
        (ph_os_load(&recoveries)&&(uint32_t)(now-ph_os_load(&last_recovery))<LINK_COOLDOWN_MS)){
        ph_os_store(&requested,0u);
        if(!allowed)ph_os_store(&rearm,1u);
        return false;
    }
    uint32_t expected=1;ph_os_cas32(&requested,&expected,2u);if(expected!=1u)return false;
    ph_os_store(&last_recovery,now);
    ph_os_store(&recoveries,ph_os_load(&recoveries)+1u);ph_os_store(&requested,0u);return true;
}
#else
/* POSIX service tests do not impersonate the SDK's Ethernet interface. */
void PH_BL_LinkPoll(bool ready){(void)ready;}
void PH_BL_LinkRx(void *interface,bool group,bool accepted)
{(void)interface;(void)group;(void)accepted;}
ph_link_stats PH_BL_LinkSnapshot(void){ph_link_stats s={0};return s;}
bool PH_BL_LinkTakeRecovery(bool allowed){(void)allowed;return false;}
#endif

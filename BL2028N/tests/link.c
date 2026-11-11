/* Exercise the production detector with a serialized, fault-injected lwIP. */
#include "link_sdk_stub.h"
#include "../src/ph_link.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static struct netif station={{0x01020304},{0x01020301},true,true},ap;
static uint32_t clock_ms;
static unsigned submissions,callbacks;
static bool in_tcpip,arp_replies;
static int arp_error,callback_error;
static void (*queued)(void *);
uint32_t rtos_get_time(void){return clock_ms;}
void *net_get_sta_handle(void){return &station;}
err_t tcpip_try_callback(void (*fn)(void *),void *arg)
{
    assert(!in_tcpip&&!arg);if(callback_error)return callback_error;
    assert(!queued);queued=fn;callbacks++;return ERR_OK;
}
err_t etharp_request(struct netif *n,const ip4_addr_t *ip)
{
    assert(in_tcpip&&n==&station&&ip->addr==station.gateway.addr);submissions++;
    if(arp_replies&&!arp_error)PH_BL_LinkRx(n,false,true);
    return arp_error;
}
static void drain(void)
{if(queued){void (*fn)(void *)=queued;queued=NULL;in_tcpip=true;fn(NULL);in_tcpip=false;}}
static void tick(bool ready)
{clock_ms+=1000;PH_BL_LinkPoll(ready);drain();}
static void seconds(unsigned count,bool ready)
{for(unsigned i=0;i<count;i++){tick(ready);assert(!PH_BL_LinkTakeRecovery(true));}}
static void begin(void){PH_BL_LinkPoll(true);drain();}
static void deaf_until_request(void)
{
    begin();
    for(unsigned i=0;i<44;i++){
        PH_BL_LinkRx(&station,true,true); /* mDNS/ARP broadcast is insufficient. */
        PH_BL_LinkRx(&station,false,false); /* Failed lwIP admission is insufficient. */
        PH_BL_LinkRx(&ap,false,true); /* An AP client is not the station. */
        tick(true);assert(!PH_BL_LinkTakeRecovery(true));
    }
    assert(PH_BL_LinkSnapshot().probes==3);tick(true);
}
int main(int argc,char **argv)
{
    assert(argc==2);unsigned scenario=(unsigned)atoi(argv[1]);
    switch(scenario){
    case 0: /* A silent, working LAN, including no MQTT configuration. */
        arp_replies=true;begin();seconds(240,true);
        assert(submissions>3&&PH_BL_LinkSnapshot().recoveries==0);break;
    case 1: /* Reproduce the observed associated, broadcast-only RX state. */
        deaf_until_request();assert(PH_BL_LinkTakeRecovery(true));
        assert(!PH_BL_LinkTakeRecovery(true));
        assert(PH_BL_LinkSnapshot().recoveries==1);
        assert(PH_BL_LinkSnapshot().unicast_rx==0&&PH_BL_LinkSnapshot().group_rx==44);
        assert(PH_BL_LinkSnapshot().input_errors==44);break;
    case 2: /* Unicast resumes before the native owner consumes the request. */
        deaf_until_request();PH_BL_LinkRx(&station,false,true);
        assert(!PH_BL_LinkTakeRecovery(true));arp_replies=true;seconds(100,true);break;
    case 3: /* Local TX allocation/queue failure must not diagnose link failure. */
        arp_error=ERR_MEM;begin();seconds(240,true);
        assert(submissions<=44&&PH_BL_LinkSnapshot().probes==0);
        assert(PH_BL_LinkSnapshot().probe_errors==submissions);
        arp_error=0;seconds(19,true);tick(true);assert(PH_BL_LinkTakeRecovery(true));break;
    case 4: /* OTA/scan/provisioning readiness and missing STA address/gateway. */
        PH_BL_LinkPoll(false);drain();seconds(100,false);assert(!submissions);
        station.gateway.addr=0;begin();seconds(100,true);assert(!submissions);
        station.gateway.addr=1;station.ip.addr=0;seconds(100,true);assert(!submissions);
        station.ip.addr=2;station.link=false;seconds(100,true);assert(!submissions);
        station.link=true;station.up=false;seconds(100,true);assert(!submissions);
        station.up=true;tick(true);seconds(44,true);tick(true);
        PH_BL_LinkPoll(false); /* Cancel immediately, even before queued work. */
        assert(!PH_BL_LinkTakeRecovery(true));drain();assert(!PH_BL_LinkSnapshot().recoveries);break;
    case 5: /* A blocked recovery is discarded; a fresh probe sequence is required. */
        deaf_until_request();assert(!PH_BL_LinkTakeRecovery(false));
        tick(false);begin();seconds(44,true);tick(true);
        assert(PH_BL_LinkTakeRecovery(true));tick(false);begin();
        seconds(300,true);assert(PH_BL_LinkSnapshot().recoveries==1);
        {
            bool recovered=false;
            for(unsigned i=0;i<60&&!recovered;i++){tick(true);recovered=PH_BL_LinkTakeRecovery(true);}
            assert(recovered&&PH_BL_LinkSnapshot().recoveries==2);
        }
        break;
    case 6: /* IP/gateway changes receive a full grace period and new probes. */
        begin();seconds(40,true);station.ip.addr++;tick(true);seconds(40,true);
        station.gateway.addr++;tick(true);seconds(44,true);tick(true);
        assert(PH_BL_LinkTakeRecovery(true));break;
    case 7: /* Beken milliseconds wrap after about 49 days. */
        clock_ms=UINT32_MAX-20000;deaf_until_request();assert(PH_BL_LinkTakeRecovery(true));break;
    case 8: /* At most one callback; queue failure and stale queued work are safe. */
        PH_BL_LinkPoll(true);
        for(unsigned i=0;i<10000;i++)PH_BL_LinkPoll(true);
        assert(callbacks==1);PH_BL_LinkPoll(false);drain();
        callback_error=ERR_MEM;seconds(60,true);assert(!submissions&&!PH_BL_LinkSnapshot().recoveries);
        callback_error=0;arp_replies=true;seconds(120,true);assert(submissions&&callbacks<130);break;
    case 9: /* A short maintenance pause can happen between LAN polls. */
        deaf_until_request();assert(!PH_BL_LinkTakeRecovery(false));
        tick(true);seconds(44,true);tick(true);assert(PH_BL_LinkTakeRecovery(true));break;
    default:assert(!"unknown scenario");
    }
    printf("PASS link scenario %u: ARP submissions=%u, recoveries=%u\n",scenario,submissions,PH_BL_LinkSnapshot().recoveries);
}

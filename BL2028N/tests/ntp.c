/* Real UDP peer drives the production SNTP owner, with deterministic DNS/time. */
#include "platform_host.h"
#include <assert.h>
static uint32_t test_ms,test_utc,test_random;
static unsigned syncs,locks;
static uint16_t server_port;
static int test_connect(int,const struct sockaddr *,socklen_t);
static struct hostent *test_resolve(const char *);
#define connect test_connect
#define gethostbyname test_resolve
#include "../src/ph_ntp.c"
#undef connect
#undef gethostbyname
ph_device bl_device;
uint32_t rtos_get_time(void){return test_ms;}
OSStatus BkRandomNumberRead(void *out,int n){assert(n==4);uint32_t v=++test_random;memcpy(out,&v,4);return 0;}
void bl_lock(void){assert(!locks);++locks;}
void bl_unlock(void){assert(locks==1);--locks;}
void ph_clock_sync(ph_device *d,uint32_t utc){assert(d==&bl_device&&locks==1);test_utc=utc;++syncs;}
static struct hostent *test_resolve(const char *name){
    assert(name&&*name);return gethostbyname("127.0.0.1");
}
static int test_connect(int fd,const struct sockaddr *p,socklen_t n){
    assert(n==sizeof(struct sockaddr_in));struct sockaddr_in a=*(const struct sockaddr_in *)p;
    assert(ntohs(a.sin_port)==123);a.sin_port=htons(server_port);return connect(fd,(struct sockaddr *)&a,sizeof(a));
}
static int server;
static unsigned char request[48];
static struct sockaddr_in client;
static void receive_request(void){
    socklen_t n=sizeof(client);assert(recvfrom(server,request,sizeof(request),0,(struct sockaddr *)&client,&n)==48);assert(request[0]==0x23);
}
static void response(unsigned char packet[48],uint32_t utc){
    memset(packet,0,48);packet[0]=0x24;packet[1]=2;memcpy(packet+24,request+40,8);
    uint32_t seconds=utc+UINT32_C(2208988800);packet[40]=seconds>>24;packet[41]=seconds>>16;packet[42]=seconds>>8;packet[43]=seconds;
}
static void reply(unsigned char packet[48]){assert(sendto(server,packet,48,0,(struct sockaddr *)&client,sizeof(client))==48);}
int main(void){
    server=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);assert(server>=0);
    struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(!bind(server,(struct sockaddr *)&a,sizeof(a)));
    socklen_t n=sizeof(a);assert(!getsockname(server,(struct sockaddr *)&a,&n));server_port=ntohs(a.sin_port);
    struct timeval timeout={2,0};assert(!setsockopt(server,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));
    ph_ntp_poll("clock.example",true);receive_request();assert(!syncs);
    unsigned char packet[48];uint32_t utc;
    response(packet,1800000000);
    int rogue=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);assert(rogue>=0);
    assert(sendto(rogue,packet,48,0,(struct sockaddr *)&client,sizeof(client))==48);
    ph_ntp_poll("clock.example",true);assert(!syncs);close(rogue);
    assert(sendto(server,packet,47,0,(struct sockaddr *)&client,sizeof(client))==47);
    ph_ntp_poll("clock.example",true);assert(!syncs);
    for(unsigned bad=0;bad<7;++bad){
        response(packet,1800000000);
        switch(bad){case 0:packet[24]^=1;break;case 1:packet[0]|=0xc0;break;case 2:packet[1]=0;break;case 3:packet[1]=16;break;case 4:packet[0]=0x23;break;case 5:packet[0]=0x1c;break;case 6:memset(packet+40,0,8);break;}
        assert(!ph_ntp_reply(packet,48,ntp_nonce,&utc));reply(packet);ph_ntp_poll("clock.example",true);assert(!syncs);
    }
    response(packet,1800000000);assert(!ph_ntp_reply(packet,47,ntp_nonce,&utc));reply(packet);ph_ntp_poll("clock.example",true);
    assert(syncs==1&&test_utc==1800000000&&ntp_fd<0);
    test_ms=3599999;ph_ntp_poll("clock.example",true);assert(ntp_fd<0);
    test_ms=3600000;ph_ntp_poll("clock.example",true);receive_request();assert(ntp_fd>=0);
    test_ms+=5000;ph_ntp_poll("clock.example",true);assert(ntp_fd<0);
    test_ms+=55000;ph_ntp_poll("clock.example",true);receive_request();assert(ntp_fd>=0);
    ph_ntp_poll("clock.example",false);assert(ntp_fd<0);
    ph_ntp_poll("other.example",true);receive_request();assert(ntp_fd>=0);
    response(packet,2147483600u);reply(packet);ph_ntp_poll("other.example",true);assert(syncs==2&&test_utc==2147483600u); /* NTP era 1, after 2036 */
    ph_ntp_poll("other.example",false);close(server);
    puts("PASS: real UDP SNTP, nonce/source policy, malformed replies, timeouts, retries, offline teardown, server change and 2036 era");
}

/* Linux mac80211 EasyLink V2 injection sender, derived from the verified M1 test. */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static int monitor_mode;
static void interrupted(int n) { (void)n; stop=1; }
static double monotonic(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static int line(char *s, size_t n) {
    if (!fgets(s,n,stdin)) return -1;
    size_t l=strlen(s);
    if (l && s[l-1]=='\n') s[--l]=0;
    else if (!feof(stdin)) return -1;
    if (l && s[l-1]=='\r') s[--l]=0;
    return (int)l;
}
static void put16(unsigned char *p, unsigned v) { p[0]=v>>8; p[1]=v; }
static uint16_t ip_checksum(const unsigned char *p) {
    unsigned sum=0;
    for (unsigned i=0;i<20;i+=2) sum+=(p[i]<<8)|p[i+1];
    while(sum>>16) sum=(sum&65535)+(sum>>16);
    return (uint16_t)~sum;
}
static int transmit(int fd, const struct sockaddr_ll *base, const unsigned char mac[6],
                    const unsigned char src[4], unsigned second, unsigned third,
                    unsigned fourth, unsigned payload, unsigned sequence) {
    unsigned char frame[256]={0};
    unsigned char dst[6]={1,0,0x5e,second&127,third,fourth};
    size_t prefix=14;
    if(monitor_mode) {
        /* Radiotap: legacy 1 Mbps, no ACK. Followed by FromDS data + LLC. */
        static const unsigned char radiotap[12]={0,0,12,0,4,128,0,0,2,0,8,0};
        static const unsigned char llc[8]={0xaa,0xaa,3,0,0,0,8,0};
        memcpy(frame,radiotap,sizeof(radiotap));
        unsigned char *w=frame+12;
        w[0]=8;w[1]=2;
        memcpy(w+4,dst,6);memcpy(w+10,mac,6);memcpy(w+16,mac,6);
        unsigned sc=(sequence&4095)<<4;w[22]=sc;w[23]=sc>>8;
        memcpy(w+24,llc,sizeof(llc));prefix=12+24+8;
    } else {
        memcpy(frame,dst,6); memcpy(frame+6,mac,6); put16(frame+12,ETH_P_IP);
    }
    unsigned char *ip=frame+prefix, *udp=ip+20;
    ip[0]=0x45; put16(ip+2,20+8+payload); put16(ip+4,sequence);
    ip[8]=1; ip[9]=17; memcpy(ip+12,src,4);
    ip[16]=239;ip[17]=second;ip[18]=third;ip[19]=fourth;
    put16(ip+10,ip_checksum(ip));
    put16(udp,49152);put16(udp+2,10000+sequence%50000);put16(udp+4,8+payload);
    struct sockaddr_ll target=*base;
    memcpy(target.sll_addr,dst,6); target.sll_halen=6;
    size_t len=prefix+20+8+payload;
    /* A scan can stall the driver's TX queue; never block beyond the test window. */
    ssize_t n=sendto(fd,frame,len,MSG_DONTWAIT,(const struct sockaddr *)&target,sizeof(target));
    if(n!=(ssize_t)len) { perror("sendto"); return -1; }
    struct timespec delay={0,10000000};
    nanosleep(&delay,NULL);
    return 0;
}
int main(int argc,char **argv) {
    if(argc!=4) { fprintf(stderr,"usage: %s INTERFACE SOURCE_IP SECONDS (SSID/password prompted); --fixture reads SSID and key on stdin for offline encoding\n",argv[0]);return 2; }
    unsigned char src[4], record[128];char ssid[130],key[130],*end;
    if(inet_pton(AF_INET,argv[2],src)!=1)return 2;
    double duration=strtod(argv[3],&end);
    if(*end || !(duration>=1 && duration<=600))return 2;
    fprintf(stderr,"Wi-Fi SSID: ");
    int sn=line(ssid,sizeof(ssid));
    char *secret;
    if(!strcmp(argv[1],"--fixture")) {
        if(line(key,sizeof(key))<0)return 2;
        secret=key;
    } else secret=getpass("Wi-Fi password (empty for open network): ");
    if(!secret || strlen(secret)>=sizeof(key))return 2;
    if(secret!=key) { strcpy(key,secret); memset(secret,0,strlen(secret)); }
    int kn=(int)strlen(key);
    if(sn<1 || sn>32 || kn<0 || (kn && (kn<8 || kn>64))) {fprintf(stderr,"invalid SSID/key lengths\n");return 2;}
    if(kn==64)for(int i=0;i<kn;i++)if(!((key[i]>='0'&&key[i]<='9')||(key[i]>='a'&&key[i]<='f')||(key[i]>='A'&&key[i]<='F')))return 2;
    size_t n=0; record[n++]=sn;record[n++]=kn;
    memcpy(record+n,ssid,sn);n+=sn;memcpy(record+n,key,kn);n+=kn;
    if(n&1)record[n++]=0;
    record[n++]=5;record[n++]=0;record[n++]='#';memcpy(record+n,src,4);n+=4;
    if(n&1)record[n++]=0;
    memset(key,0,sizeof(key));
    if(!strcmp(argv[1],"--fixture")) {
        for(size_t i=0;i<n;i++) printf("%02x",record[i]);
        puts("");
        return 0;
    }
    int fd=socket(AF_PACKET,SOCK_RAW,htons(ETH_P_ALL));if(fd<0){perror("socket");return 1;}
    struct ifreq req={0};
    if(strlen(argv[1])>=IFNAMSIZ)return 2;
    strcpy(req.ifr_name,argv[1]);
    if(ioctl(fd,SIOCGIFHWADDR,&req)<0){perror("interface MAC");close(fd);return 1;}
    /* ARPHRD_IEEE80211_RADIOTAP = 803; ordinary AP uses Ethernet. */
    monitor_mode=req.ifr_hwaddr.sa_family==803;
    if(!monitor_mode){fprintf(stderr,"requires a mac80211 monitor interface with radiotap injection support\n");close(fd);return 1;}
    unsigned char mac[6];memcpy(mac,req.ifr_hwaddr.sa_data,6);
    unsigned idx=if_nametoindex(argv[1]);if(!idx){perror("interface index");close(fd);return 1;}
    struct sockaddr_ll target={.sll_family=AF_PACKET,.sll_protocol=htons(monitor_mode?ETH_P_ALL:ETH_P_IP),.sll_ifindex=(int)idx};
    if(bind(fd,(struct sockaddr *)&target,sizeof(target))<0){perror("bind");close(fd);return 1;}
    signal(SIGINT,interrupted);signal(SIGTERM,interrupted);
    double start=monotonic(), report=start;unsigned count=0,cycles=0;
    printf("Starting V2 on %s (%s), %.0f seconds, %zu record bytes; no receiver acknowledgement implied\n",argv[1],monitor_mode?"radiotap injection":"Ethernet AP",duration,n);fflush(stdout);
    while(!stop && monotonic()-start<duration) {
        for(unsigned i=0;i<20 && !stop && monotonic()-start<duration;i++) {
            if(transmit(fd,&target,mac,src,118,0,0,20,count)<0){close(fd);return 1;} count++;
        }
        for(size_t i=0;i<n && !stop && monotonic()-start<duration;i+=2) {
            for(unsigned j=0;j<3 && !stop && monotonic()-start<duration;j++) {
                if(transmit(fd,&target,mac,src,126,record[i],record[i+1],20+i/2,count)<0){close(fd);return 1;}count++;
            }
        }
        cycles++;
        if(monotonic()-report>=5){printf("packets=%u cycles=%u elapsed=%.1f\n",count,cycles,monotonic()-start);fflush(stdout);report=monotonic();}
    }
    printf("Stopped packets=%u cycles=%u elapsed=%.1f\n",count,cycles,monotonic()-start);
    memset(record,0,sizeof(record));close(fd);return 0;
}

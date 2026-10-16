#include "mico.h"
#include "fetch.h"
#include "mode.h"
#include "tls_profile.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    int fd;
    bool tls;
    uint32_t deadline;
    ota_tls_context *ssl;
    unsigned char *buffer;
    char host[128], path[384];
    unsigned port;
} transfer;
static bool parse_url(const char *url, transfer *t) {
    if (!url || strlen(url)>=500 || strpbrk(url,"\r\n\t @#\\")) return false;
    unsigned prefix=!strncmp(url,"https://",8)?8:!strncmp(url,"http://",7)?7:0;
    if (!prefix)return false;
    t->tls=prefix==8;t->port=t->tls?443:80;
    const char *start=url+prefix,*end=strchr(start,'/');
    size_t n=end?(size_t)(end-start):strlen(start);
    if(!n||n>=sizeof(t->host))return false;
    memcpy(t->host,start,n);t->host[n]=0;
    if(end && strlen(end)>=sizeof(t->path))return false;
    strcpy(t->path,end?end:"/");
    char *colon=strchr(t->host,':');
    if(colon){char *last;unsigned long p=strtoul(colon+1,&last,10);if(!p||p>65535||*last)return false;*colon=0;t->port=p;}
    if(!t->host[0]||strchr(t->host,'?'))return false;
    for(char *p=t->host;*p;p++)if(!isalnum((unsigned char)*p)&&*p!='.'&&*p!='-')return false;
    return true;
}
bool ota_url_valid(const char *url) {transfer t;return parse_url(url,&t);}
static int socket_io(transfer *t, void *data, size_t n, bool writing) {
    ota_job_sample();
    uint32_t start=mico_rtos_get_time();
    do{
        if(ota_job_cancelled()||(int32_t)(t->deadline-mico_rtos_get_time())<=0)return -1;
        fd_set f;FD_ZERO(&f);FD_SET(t->fd,&f);struct timeval tv={0,200000};
        int rc=select(t->fd+1,writing?NULL:&f,writing?&f:NULL,NULL,&tv);
        if(rc<0)return -1;
        if(rc){int nbytes=writing?send(t->fd,data,n,0):recv(t->fd,data,n,0);return nbytes>0?nbytes:-1;}
    }while((uint32_t)(mico_rtos_get_time()-start)<10000);
    return -1;
}
static int tls_read(void *v,unsigned char *p,size_t n){return socket_io(v,p,n,false);}
static int tls_write(void *v,const unsigned char *p,size_t n){return socket_io(v,(void *)p,n,true);}
static unsigned word(const unsigned char *p){return (unsigned)p[0]*256+p[1];}
static bool dns_skip(const unsigned char *p,unsigned n,unsigned *at){
    while(*at<n){unsigned c=p[(*at)++];if(!c)return true;if((c&0xc0)==0xc0){if(*at>=n)return false;(*at)++;return true;}if(c>63||*at+c>n)return false;*at+=c;}return false;
}
static bool resolve_host(transfer *t,struct in_addr *addr){
    uint32_t literal=inet_addr(t->host);if(literal!=INADDR_NONE){addr->s_addr=literal;return true;}
    IPStatusTypedef ip;if(micoWlanGetIPStatus(&ip,Station)!=kNoErr)return false;
    unsigned char packet[512]={0},id[2];if(MicoRandomNumberRead(id,2)!=kNoErr)return false;
    packet[0]=id[0];packet[1]=id[1];packet[2]=1;packet[5]=1;
    unsigned at=12;const char *label=t->host;
    while(*label){const char *dot=strchr(label,'.');unsigned n=dot?(unsigned)(dot-label):strlen(label);if(!n||n>63||at+n+5>sizeof(packet))return false;packet[at++]=n;memcpy(packet+at,label,n);at+=n;label+=n;if(*label)label++;}
    packet[at++]=0;packet[at++]=0;packet[at++]=1;packet[at++]=0;packet[at++]=1;
    struct sockaddr_in server={0};server.sin_family=AF_INET;server.sin_port=htons(53);server.sin_addr.s_addr=inet_addr(ip.dns);
    if(server.sin_addr.s_addr==INADDR_NONE||!server.sin_addr.s_addr)return false;
    t->fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(t->fd<0)return false;
    bool ok=false;if(connect(t->fd,(struct sockaddr *)&server,sizeof(server))<0)goto done;
    if(socket_io(t,packet,at,true)!=(int)at)goto done;
    int count=socket_io(t,packet,sizeof(packet),false);
    if(count<12||packet[0]!=id[0]||packet[1]!=id[1]||(packet[2]&0x82)!=0x80||(packet[3]&0x0f)||word(packet+4)!=1)goto done;
    at=12;if(!dns_skip(packet,count,&at)||at+4>(unsigned)count)goto done;at+=4;
    unsigned answers=word(packet+6);
    while(answers--){if(!dns_skip(packet,count,&at)||at+10>(unsigned)count)goto done;
        unsigned type=word(packet+at),cls=word(packet+at+2),n=word(packet+at+8);at+=10;if(at+n>(unsigned)count)goto done;
        if(type==1&&cls==1&&n==4){memcpy(addr,packet+at,4);ok=true;break;}at+=n;
    }
done:close(t->fd);t->fd=-1;return ok;
}
static bool connect_host(transfer *t) {
    struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_port=htons(t->port);
    if(!resolve_host(t,&a.sin_addr)||ota_job_cancelled())return false;
    t->fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(t->fd<0)return false;
    /* Bound connect itself; select alone only bounds established I/O. */
    int flags=fcntl(t->fd,F_GETFL,0);if(flags<0||fcntl(t->fd,F_SETFL,flags|O_NONBLOCK)<0)return false;
    int rc=connect(t->fd,(struct sockaddr *)&a,sizeof(a));
    if(rc<0){uint32_t start=mico_rtos_get_time();
        do{if(ota_job_cancelled()||(uint32_t)(mico_rtos_get_time()-start)>=10000)return false;
            fd_set f;FD_ZERO(&f);FD_SET(t->fd,&f);struct timeval tv={0,200000};rc=select(t->fd+1,NULL,&f,NULL,&tv);if(rc<0)return false;
        }while(!rc);
        int e=0;socklen_t len=sizeof(e);if(getsockopt(t->fd,SOL_SOCKET,SO_ERROR,&e,&len)<0||e)return false;}
    /* Keep the connected socket nonblocking: a writable indication does not
       guarantee that send() can consume the entire TLS record without waiting. */
    if(!t->tls)return true;
    unsigned char entropy[32];if(MicoRandomNumberRead(entropy,sizeof(entropy))!=kNoErr)return false;
    ota_tls_init(t->ssl,t->buffer,2048);br_ssl_engine_inject_entropy(&t->ssl->client.eng,entropy,sizeof(entropy));
    if(!br_ssl_client_reset(&t->ssl->client,t->host,0))return false;
    br_sslio_init(&t->ssl->io,&t->ssl->client.eng,tls_read,t,tls_write,t);return true;
}
static int read_data(transfer *t,void *p,size_t n){return t->tls?br_sslio_read(&t->ssl->io,p,n):socket_io(t,p,n,false);}
static bool send_data(transfer *t,const char *p,size_t n){
    if(t->tls)return br_sslio_write_all(&t->ssl->io,p,n)==0&&br_sslio_flush(&t->ssl->io)==0;
    while(n){int rc=socket_io(t,(void *)p,n,true);if(rc<=0)return false;p+=rc;n-=rc;}return true;
}
static int line_read(transfer *t,char *p,size_t cap){size_t n=0;while(n+1<cap){char c;if(read_data(t,&c,1)!=1||!c)return -1;p[n++]=c;if(c=='\n'){if(n<2||p[n-2]!='\r')return -1;p[n-2]=0;return n;}}return -1;}
static bool number(const char **p,uint32_t *n){if(!isdigit((unsigned char)**p))return false;*n=0;do{unsigned d=*(*p)++-'0';if(*n>(UINT32_MAX-d)/10)return false;*n=*n*10+d;}while(isdigit((unsigned char)**p));return true;}
static const char *space(const char *p){while(*p==' '||*p=='\t')p++;return p;}
bool ota_fetch(const char *url,uint32_t limit,ota_fetch_begin begin,ota_fetch_write write,void *ctx,const char **error){
    transfer *t=calloc(1,sizeof(*t));char line[640];unsigned char chunk[512];
    uint32_t done=0,total=0;bool connected=false,ok=false;
    *error="No memory for transfer";if(!t)return false;t->fd=-1;t->deadline=mico_rtos_get_time()+180000;
#define FAIL(s) do{*error=(s);goto out;}while(0)
    if(!write||!limit||!parse_url(url,t))FAIL("Invalid URL");
    if(t->tls){t->ssl=calloc(1,sizeof(*t->ssl));t->buffer=malloc(2048);if(!t->ssl||!t->buffer)FAIL("No memory for TLS");}
    do{
        ota_job_sample();
        if(ota_job_cancelled())FAIL("Cancelled");
        if((int32_t)(mico_rtos_get_time()-t->deadline)>=0)FAIL("Download timed out");
        if(!connected){if(!connect_host(t))FAIL("DNS, connection or TLS setup failed");connected=true;}
        uint32_t last=done+1023;if(total&&last>=total)last=total-1;
        int n=t->tls?snprintf(line,sizeof(line),"GET %s HTTP/1.1\r\nHost: %s:%u\r\nRange: bytes=%lu-%lu\r\nConnection: keep-alive\r\nAccept-Encoding: identity\r\n\r\n",t->path,t->host,t->port,(unsigned long)done,(unsigned long)last):snprintf(line,sizeof(line),"GET %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\nAccept-Encoding: identity\r\n\r\n",t->path,t->host,t->port);
        if(n<0||(size_t)n>=sizeof(line)||!send_data(t,line,n))FAIL("TLS handshake or request failed");
        if(line_read(t,line,sizeof(line))<0||strncmp(line,"HTTP/1.",7)||line[8]!=' ')FAIL("Invalid HTTP status");
        unsigned status=atoi(line+9),headers=0;uint32_t length=0,first=0,end=0,size=0;bool have=false,range=false,close_peer=false;
        for(;;){n=line_read(t,line,sizeof(line));if(n<0||(headers+=n)>8192)FAIL("Invalid HTTP headers");if(n==2)break;
            const char *p;
            if(!strncasecmp(line,"Content-Length:",15)){p=space(line+15);if(have||!number(&p,&length)||*space(p)||!length)FAIL("Invalid content length");have=true;}
            else if(!strncasecmp(line,"Content-Range:",14)){p=space(line+14);if(range||strncmp(p,"bytes ",6))FAIL("Invalid range");p+=6;if(!number(&p,&first)||*p++!='-'||!number(&p,&end)||*p++!='/'||!number(&p,&size)||*space(p))FAIL("Invalid range");range=true;}
            else if(!strncasecmp(line,"Transfer-Encoding:",18)||!strncasecmp(line,"Content-Encoding:",17))FAIL("Encoded responses unsupported");
            else if(!strncasecmp(line,"Connection:",11)&&!strcasecmp(space(line+11),"close"))close_peer=true;
        }
        if(!have)FAIL("Missing content length");
        if(t->tls){if(status!=206||!range||!size||size>limit||(total&&size!=total)||first!=done||end!=(last<size?last:size-1)||end<first||length!=end-first+1)FAIL("Range response rejected");}
        else{if(status!=200||range||length>limit)FAIL("HTTP response rejected");size=length;}
        if(!total){total=size;if(begin&&!begin(ctx,total))FAIL("Package rejected");}
        while(length){if(ota_job_cancelled())FAIL("Cancelled");
        if((int32_t)(mico_rtos_get_time()-t->deadline)>=0)FAIL("Download timed out");n=read_data(t,chunk,length>sizeof(chunk)?sizeof(chunk):length);if(n<=0)FAIL("Download interrupted");if(!write(ctx,done,chunk,n))FAIL("Signature or write rejected");done+=n;length-=n;}
        if(close_peer){close(t->fd);t->fd=-1;connected=false;}
    }while(done<total);
    ok=true;*error=NULL;
out:
    if(!ok && ota_job_cancelled())*error="Cancelled";
    if(t->fd>=0)close(t->fd);free(t->buffer);free(t->ssl);free(t);return ok;
#undef FAIL
}

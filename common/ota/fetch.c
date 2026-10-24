#include "mico.h"
#include "fetch.h"
#include "mode.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifndef OTA_FETCH_HANDSHAKE_MS
#define OTA_FETCH_HANDSHAKE_MS 10000u
#endif
#ifndef OTA_FETCH_TOTAL_MS
#define OTA_FETCH_TOTAL_MS 180000u
#endif
/* Pinned MK3080B kernel 3080B002.024 consumes the fd in ssl_close, whereas
 * MK3031 kernel 30300002.015 leaves it with the caller. Product wrappers must
 * select the installed kernel ABI, not an unrelated SDK wolfSSL archive. */
#ifndef OTA_TLS_CLOSES_SOCKET
#define OTA_TLS_CLOSES_SOCKET 1
#endif

typedef struct {
    int fd;
    bool tls;
    uint32_t deadline;
    mico_ssl_t ssl;
    mico_semaphore_t handshake_stop, handshake_done;
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
/* MiCO's closed socket kernel does not export its errno store to newlib.
 * SO_ERROR and bounded retries work for both the SDK and the POSIX adapter. */
static bool socket_failed(transfer *t) {
    int error=0;socklen_t length=sizeof(error);
    return getsockopt(t->fd,SOL_SOCKET,SO_ERROR,&error,&length)<0 || error!=0;
}
static int socket_io(transfer *t, void *data, size_t n, bool writing) {
    ota_job_sample();
    uint32_t start=mico_rtos_get_time();
    do{
        if(ota_job_cancelled()||(int32_t)(t->deadline-mico_rtos_get_time())<=0)return -1;
        fd_set f;FD_ZERO(&f);FD_SET(t->fd,&f);struct timeval tv={0,200000};
        int rc=select(t->fd+1,writing?NULL:&f,writing?&f:NULL,NULL,&tv);
        if(rc<0){if(socket_failed(t))return -1;mico_thread_msleep(20);continue;}
        if(rc){int nbytes=writing?send(t->fd,data,n,0):recv(t->fd,data,n,0);
            if(nbytes>=0)return nbytes?nbytes:-1;
            if(socket_failed(t))return -1;mico_thread_msleep(20);}
    }while((uint32_t)(mico_rtos_get_time()-start)<10000);
    return -1;
}
/* Kernel TLS owns its record buffers and cryptography. The observer bounds a
 * stalled SNI handshake without freeing a socket while the kernel uses it. */
static void handshake_observer(mico_thread_arg_t arg) {
    transfer *t=(transfer *)arg;
    uint32_t start=mico_rtos_get_time();
    while(mico_rtos_get_semaphore(&t->handshake_stop,20)!=kNoErr) {
        if(ota_job_cancelled() || (uint32_t)(mico_rtos_get_time()-start)>=OTA_FETCH_HANDSHAKE_MS ||
           (int32_t)(t->deadline-mico_rtos_get_time())<=0) {
            /* mocIP forwards this directly to lwIP, whose ABI uses 2. The
             * public MiCO header incorrectly defines SHUT_RDWR as 3. */
            shutdown(t->fd,2);break;
        }
    }
    mico_rtos_set_semaphore(&t->handshake_done);
    mico_rtos_delete_thread(NULL);
}
static bool kernel_tls_connect(transfer *t) {
    if(mico_rtos_init_semaphore(&t->handshake_stop,1)!=kNoErr)return false;
    if(mico_rtos_init_semaphore(&t->handshake_done,1)!=kNoErr) {
        mico_rtos_deinit_semaphore(&t->handshake_stop);return false;
    }
    bool started=mico_rtos_create_thread(NULL,MICO_APPLICATION_PRIORITY,"ota_tls_guard",
        handshake_observer,1024,(mico_thread_arg_t)t)==kNoErr;
    if(started) {
        int error=-1,flags=fcntl(t->fd,F_GETFL,0);
        /* A cross-thread lwIP shutdown need not wake an already blocked recv.
         * Bound each blocking handshake operation as well (SDK uses ms ints). */
        int io_timeout=1000;
        if(setsockopt(t->fd,SOL_SOCKET,SO_RCVTIMEO,&io_timeout,sizeof(io_timeout))<0 ||
           setsockopt(t->fd,SOL_SOCKET,SO_SNDTIMEO,&io_timeout,sizeof(io_timeout))<0)flags=-1;
        if(flags>=0 && fcntl(t->fd,F_SETFL,flags&~O_NONBLOCK)>=0) {
            ssl_set_client_version(TLS_V1_2_MODE);
            /* Preserve the signed-OTA trust policy: encrypted transport with
             * SNI, no CA validation; the package authenticates the publisher. */
            t->ssl=ssl_connect_sni(t->fd,0,NULL,t->host,&error);
        }
        mico_rtos_set_semaphore(&t->handshake_stop);
        mico_rtos_get_semaphore(&t->handshake_done,MICO_WAIT_FOREVER);
        /* MK3080B also consumes the fd on handshake failure after set_fd.
         * Early context/SNI allocation errors leave it with the caller. */
        if(OTA_TLS_CLOSES_SOCKET && !t->ssl && error!=-1 && error!=-203 && error!=-125)t->fd=-1;
        if(t->ssl) {
            ssl_set_using_nonblock(t->ssl,1);
            if(fcntl(t->fd,F_SETFL,flags|O_NONBLOCK)<0) {
                ssl_close(t->ssl);t->ssl=NULL;
                if(OTA_TLS_CLOSES_SOCKET)t->fd=-1;
            }
        }
    }
    mico_rtos_deinit_semaphore(&t->handshake_done);
    mico_rtos_deinit_semaphore(&t->handshake_stop);
    ota_job_sample();
    return t->ssl && !ota_job_cancelled();
}
static int kernel_tls_io(transfer *t,void *data,size_t n,bool writing) {
    uint32_t start=mico_rtos_get_time();bool wait_write=writing;
    for(;;) {
        ota_job_sample();
        if(ota_job_cancelled() || (int32_t)(t->deadline-mico_rtos_get_time())<=0 ||
           (uint32_t)(mico_rtos_get_time()-start)>=10000u)return -1;
        if(wait_write || ssl_pending(t->ssl)<=0) {
            fd_set f;FD_ZERO(&f);FD_SET(t->fd,&f);struct timeval tv={0,200000};
            int ready=select(t->fd+1,wait_write?NULL:&f,wait_write?&f:NULL,NULL,&tv);
            if(ready<0){if(socket_failed(t))return -1;mico_thread_msleep(20);continue;}
            if(!ready)continue;
        }
        int rc=writing?ssl_send(t->ssl,data,n):ssl_recv(t->ssl,data,n);
        if(rc>0)return rc;
        int error=ssl_get_error(t->ssl,rc);
        /* MiCO's kernel exports the OpenSSL-compatible WANT_READ/WRITE codes. */
        if(error==2)wait_write=false;
        else if(error==3)wait_write=true;
        else return -1;
    }
}
static void disconnect_host(transfer *t) {
    if(t->ssl){ssl_close(t->ssl);t->ssl=NULL;if(OTA_TLS_CLOSES_SOCKET)t->fd=-1;}
    if(t->fd>=0){close(t->fd);t->fd=-1;}
}
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
        do{if(ota_job_cancelled()||(uint32_t)(mico_rtos_get_time()-start)>=10000 ||
              (int32_t)(t->deadline-mico_rtos_get_time())<=0)return false;
            fd_set f;FD_ZERO(&f);FD_SET(t->fd,&f);struct timeval tv={0,200000};rc=select(t->fd+1,NULL,&f,NULL,&tv);
            if(rc<0){if(socket_failed(t))return false;mico_thread_msleep(20);rc=0;}
        }while(!rc);
        if(socket_failed(t))return false;}
    /* Keep the connected socket nonblocking: a writable indication does not
       guarantee that send() can consume the entire TLS record without waiting. */
    if(!t->tls)return true;
    return kernel_tls_connect(t);
}
static int read_data(transfer *t,void *p,size_t n){return t->tls?kernel_tls_io(t,p,n,false):socket_io(t,p,n,false);}
static bool send_data(transfer *t,const char *p,size_t n){
    while(n){int rc=t->tls?kernel_tls_io(t,(void *)p,n,true):socket_io(t,(void *)p,n,true);if(rc<=0)return false;p+=rc;n-=rc;}return true;
}
static int line_read(transfer *t,char *p,size_t cap){size_t n=0;while(n+1<cap){char c;if(read_data(t,&c,1)!=1||!c)return -1;p[n++]=c;if(c=='\n'){if(n<2||p[n-2]!='\r')return -1;p[n-2]=0;return n;}}return -1;}
static bool number(const char **p,uint32_t *n){if(!isdigit((unsigned char)**p))return false;*n=0;do{unsigned d=*(*p)++-'0';if(*n>(UINT32_MAX-d)/10)return false;*n=*n*10+d;}while(isdigit((unsigned char)**p));return true;}
static const char *space(const char *p){while(*p==' '||*p=='\t')p++;return p;}
bool ota_fetch(const char *url,uint32_t limit,ota_fetch_begin begin,ota_fetch_write write,void *ctx,const char **error){
    transfer *t=calloc(1,sizeof(*t));char line[640];unsigned char chunk[512];
    uint32_t done=0,total=0;bool connected=false,ok=false;
    *error="No memory for transfer";if(!t)return false;t->fd=-1;t->deadline=mico_rtos_get_time()+OTA_FETCH_TOTAL_MS;
#define FAIL(s) do{*error=(s);goto out;}while(0)
    if(!write||!limit||!parse_url(url,t))FAIL("Invalid URL");
    do{
        ota_job_sample();
        if(ota_job_cancelled())FAIL("Cancelled");
        if((int32_t)(mico_rtos_get_time()-t->deadline)>=0)FAIL("Download timed out");
        if(!connected){if(!connect_host(t))FAIL("DNS, connection or TLS setup failed");connected=true;}
        uint32_t last=done+1023;if(total&&last>=total)last=total-1;
        int n=t->tls?snprintf(line,sizeof(line),"GET %s HTTP/1.1\r\nHost: %s:%u\r\nRange: bytes=%lu-%lu\r\nConnection: keep-alive\r\nAccept-Encoding: identity\r\n\r\n",t->path,t->host,t->port,(unsigned long)done,(unsigned long)last):snprintf(line,sizeof(line),"GET %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\nAccept-Encoding: identity\r\n\r\n",t->path,t->host,t->port);
        if(n<0||(size_t)n>=sizeof(line)||!send_data(t,line,n))FAIL("TLS handshake or request failed");
        if(line_read(t,line,sizeof(line))<12||strncmp(line,"HTTP/1.",7)||line[8]!=' ')FAIL("Invalid HTTP status");
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
        if(close_peer){disconnect_host(t);connected=false;}
    }while(done<total);
    ok=true;*error=NULL;
out:
    if(!ok && ota_job_cancelled())*error="Cancelled";
    else if(!ok && (int32_t)(t->deadline-mico_rtos_get_time())<=0)*error="Download timed out";
    disconnect_host(t);free(t);return ok;
#undef FAIL
}

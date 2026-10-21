/* Executes the production parser and scheduler, including callback lifetimes. */
#include "http.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static ph_http_server s;
static unsigned tick, releases, begins, completes, bodies, yields, sent_ok;
static int child, error_value, fail_limit, fail_nonblock, fail_close, fail_wait, fail_accept;
static int transient_send, transient_recv, closes[80];
static const char *input[80]; static size_t positions[80];
static char output[80][4096]; static size_t lengths[80];
static uint64_t live;
static bool body_fail;
static unsigned desired=3;
static uint32_t now(void){return tick;}
static int open_listener(unsigned cap){(void)cap;live|=1;return 0;}
static int limit(int fd,unsigned cap,ph_http_diagnostics *d){assert(fd==0);d->limit=(int)cap;return fail_limit?-1:0;}
static int accept_one(int fd){assert(fd==0);if(fail_accept){error_value=9;return -1;}int n=child;child=-1;if(n>=0&&n<64)live|=UINT64_C(1)<<n;return n;}
static int nonblock(int fd){return fail_nonblock==fd?-1:0;}
static int wait_one(uint64_t r,uint64_t w,uint64_t *rr,uint64_t *ww,unsigned ms){
 assert(!(r&~live));assert(!(w&~live));tick+=ms;
 if(fail_wait){error_value=fail_wait;return -1;}
 *rr=0;*ww=w;
 if((child>=0 || fail_accept) && (r&1))*rr|=1;
 for(int i=1;i<64;i++)if((r&(UINT64_C(1)<<i))&&input[i])*rr|=UINT64_C(1)<<i;
 return *rr||*ww?1:0;
}
static int receive(int fd,void *p,size_t n){
 if(transient_recv){transient_recv--;error_value=11;return -1;}
 if(!input[fd])return 0;
 size_t remain=strlen(input[fd])-positions[fd];if(n>remain)n=remain;
 memcpy(p,input[fd]+positions[fd],n);positions[fd]+=n;return (int)n;
}
static int transmit(int fd,const void *p,size_t n){
 if(transient_send){transient_send--;error_value=11;return -1;}
 if(n>7)n=7;
 assert(lengths[fd]+n<sizeof(output[fd]));
 memcpy(output[fd]+lengths[fd],p,n);lengths[fd]+=n;return (int)n;
}
static int close_one(int fd){assert(fd>=0&&fd<80);closes[fd]++;if(fd<64)live&=~(UINT64_C(1)<<fd);if(fail_close==fd){error_value=55;return -1;}return 0;}
static bool retry(void){return error_value==11||error_value==4;}
static int get_error(void){return error_value;}
static void yield_one(void){yields++;tick++;}
static ph_http_action begin(ph_http_exchange *x){
 begins++;x->response.status=200;x->response.body="payload";x->response.length=7;
 if(!strcmp(x->request->method,"HEAD"))x->response.suppress_body=true;
 if(!strcmp(x->request->path,"/body")){x->body_total_ms=400;x->body_idle_ms=200;return PH_HTTP_READ_BODY;}
 return PH_HTTP_REPLY;
}
static ph_http_action again(ph_http_exchange *x){(void)x;return PH_HTTP_WAIT;}
static bool body(ph_http_exchange *x,const void *p,size_t n){(void)x;(void)p;assert(n<=512);bodies++;return !body_fail;}
static void complete(ph_http_exchange *x){(void)x;completes++;}
static void release(ph_http_exchange *x,bool sent){assert(x->request);releases++;if(sent)sent_ok++;}
static unsigned admission(void){return desired;}
static const ph_http_io io={now,open_listener,limit,accept_one,nonblock,wait_one,receive,transmit,close_one,retry,get_error,yield_one,NULL};
static const ph_http_app app={begin,again,body,complete,release,admission};
static void reset(void){memset(&s,0,sizeof(s));memset(input,0,sizeof(input));memset(positions,0,sizeof(positions));memset(output,0,sizeof(output));memset(lengths,0,sizeof(lengths));memset(closes,0,sizeof(closes));tick=releases=begins=completes=bodies=yields=sent_ok=0;child=-1;error_value=fail_limit=fail_wait=fail_accept=transient_send=transient_recv=0;fail_nonblock=fail_close=-1;live=0;body_fail=false;desired=3;assert(ph_http_server_open(&s,&io,&app,3));}
static void connect_one(int fd,const char *wire){child=fd;input[fd]=wire;ph_http_server_step(&s);assert(child==-1);}
static void run(unsigned n){while(n--&&s.running)ph_http_server_step(&s);}
static int parse(const unsigned char *text,size_t n){ph_http_parser p;ph_http_parser_init(&p);int rc=0;for(size_t i=0;i<n;i++){rc=ph_http_parser_feed(&p,text[i]);if(rc)break;}return rc;}
#define PARSE(x) parse((const unsigned char *)(x),sizeof(x)-1)
int main(void){
 assert(PARSE("GET / HTTP/1.1\r\n\r\n")==1);
 assert(PARSE("GET / HTTP/1.1\r\nX:a\tb\r\n\r\n")==1);
 assert(PARSE("GET / HTTP/1.1\n\n")==-1);
 assert(PARSE("GET / HTTP/1.1\rX")==-1);
 assert(PARSE("GET / HTTP/1.2\r\n\r\n")==-1);
 assert(PARSE("GET / HTTP/1.1\r\nX:a\0b\r\n\r\n")==-1);
 assert(PARSE("POST / HTTP/1.1\r\nContent-Length:4294967296\r\n\r\n")==-1);
 assert(PARSE("POST / HTTP/1.1\r\nContent-Length:1\r\nContent-Length:1\r\n\r\n")==-1);
 assert(PARSE("POST / HTTP/1.1\r\nTransfer-Encoding:identity\r\n\r\n")==-1);
 char wire[4096]="GET / HTTP/1.1\r\n";for(unsigned i=0;i<64;i++)strcat(wire,"X:a\r\n");strcat(wire,"\r\n");assert(parse((unsigned char *)wire,strlen(wire))==1);
 strcpy(wire,"GET / HTTP/1.1\r\n");for(unsigned i=0;i<65;i++)strcat(wire,"X:a\r\n");strcat(wire,"\r\n");assert(parse((unsigned char *)wire,strlen(wire))==-1);
 assert(ph_http_basic("Basic YWRtaW46cHc=","admin","pw"));assert(!ph_http_basic("","admin","pw"));
 reset();connect_one(1,"GET / HTTP/1.1\r\n\r\n");transient_recv=1;transient_send=1;run(100);assert(releases==1&&sent_ok==1&&closes[1]==1&&yields==2);assert(strstr(output[1],"Content-Length: 7\r\n")&&strstr(output[1],"\r\n\r\npayload"));ph_http_server_close(&s);assert(closes[0]==1);
 reset();connect_one(1,"GET / HTTP/1.1\r\nX:");run(1);input[1]=NULL;connect_one(2,"GET / HTTP/1.1\r\n\r\n");run(100);assert(strstr(output[2],"payload")&&begins==1&&s.slots[0].state==PH_SLOT_HEADER);tick+=5000;run(1);assert(closes[1]==1);ph_http_server_close(&s);
 reset();connect_one(1,"POST /body HTTP/1.1\r\nContent-Length:3\r\n\r\nabc");body_fail=true;run(100);assert(bodies==1&&completes==0&&releases==1&&closes[1]==1);ph_http_server_close(&s);
 reset();connect_one(1,"POST /body HTTP/1.1\r\nContent-Length:3\r\n\r\nabc");run(100);assert(bodies==1&&completes==1&&releases==1);ph_http_server_close(&s);
 reset();connect_one(1,"HEAD / HTTP/1.1\r\n\r\n");run(100);assert(strstr(output[1],"Content-Length: 7\r\n")&&!strstr(output[1],"payload"));ph_http_server_close(&s);
 reset();fail_close=1;connect_one(1,"GET / HTTP/1.1\r\n\r\n");run(100);assert(!s.running&&releases==1&&!sent_ok&&s.diagnostics.close_failures==1&&s.diagnostics.last_close_errno==55);ph_http_server_close(&s);assert(closes[1]==1&&closes[0]==1);
 reset();fail_nonblock=2;connect_one(2,"GET / HTTP/1.1\r\n\r\n");run(1);assert(closes[2]==1&&begins==0);ph_http_server_close(&s);
 reset();fail_wait=4;run(1);assert(s.running&&yields==1);fail_wait=9;run(1);assert(!s.running&&closes[0]==1);ph_http_server_close(&s);assert(closes[0]==1);
 reset();desired=2;run(1);assert(s.cap==2);fail_limit=1;desired=3;run(1);assert(!s.running&&closes[0]==1);
 reset();ph_http_server_close(&s);fail_nonblock=0;assert(!ph_http_server_open(&s,&io,&app,3));assert(closes[0]==2);
 reset();fail_accept=1;run(3);assert(!s.running&&s.diagnostics.accept_failures==3&&closes[0]==1&&yields==2);ph_http_server_close(&s);assert(closes[0]==1);
 puts("production HTTP core fault/parser tests PASS");return 0;
}

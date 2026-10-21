/* Actual MiCO port + production core: kernel errors deliberately never update
 * APP errno. Exercise all policies with stale EAGAIN, ERANGE and zero. */
#include "mico.h"
#include "http.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>
static int mode,opened,closed,deleted,configured,nonblocks,getopts,cap=3,poison;
static unsigned tick,select_calls,accept_calls,read_calls,write_calls;
static unsigned begins,releases,delivered,failed,retry_yields;
static int next_child,client_closed[6];
static size_t position[6],output_length[6];
static const char get[]="GET / HTTP/1.1\r\n\r\n";
static const char post[]="POST /body HTTP/1.1\r\nContent-Length:3\r\n\r\n";
static void stale(void){errno=poison;}
int ph_socket(int a,int b,int c){(void)a;(void)b;(void)c;stale();opened++;return mode==1?-1:3;}
int ph_bind(int fd,const struct sockaddr*a,socklen_t n){(void)a;(void)n;assert(fd==3);stale();return mode==2?-1:0;}
int ph_listen(int fd,int n){assert(fd==3&&n==0);stale();return mode==3?-1:0;}
int ph_setsockopt(int fd,int level,int option,const void*p,socklen_t n){assert(fd==3&&level==IPPROTO_TCP&&option==TCP_MAX_CONN_NUM&&n==sizeof(int));configured++;cap=*(const int*)p;stale();return mode==4?-1:0;}
int ph_getsockopt(int fd,int level,int option,void*p,socklen_t*n){assert(fd==3&&level==IPPROTO_TCP);getopts++;stale();if(mode==5)return -1;*(int*)p=option==TCP_CONN_NUM?0:mode==6?cap+1:cap;*n=sizeof(int);return 0;}
int ph_fcntl(int fd,int cmd,...){assert(fd>=3&&fd<=5);va_list a;va_start(a,cmd);int flags=va_arg(a,int);va_end(a);stale();if(mode==7)return -1;if(cmd==F_SETFL){assert(flags&O_NONBLOCK);nonblocks++;return 0;}return mode==8?0:O_NONBLOCK;}
int ph_select(int n,fd_set*r,fd_set*w,fd_set*e,struct timeval*t){
 (void)e;assert(getopts==configured*2);assert(n==64&&t->tv_usec==25000);stale();select_calls++;tick+=25;
 if(mode==18&&select_calls==2){tick+=300;FD_ZERO(r);FD_ZERO(w);return 0;}
 if(mode==18&&accept_calls==2)assert(ph_http_snapshot().accept_failures==0);
 if(mode<11 || (delivered&&mode!=15) || (mode==16&&select_calls<=2))return -1;
 fd_set reads=*r,writes=*w;FD_ZERO(r);FD_ZERO(w);
 if(FD_ISSET(3,&reads) && ((next_child<=5 && (!(mode==12||mode==14)||next_child==4||client_closed[4])) || mode==15))FD_SET(3,r);
 for(int fd=4;fd<=5;fd++)if(!client_closed[fd]){
  if(FD_ISSET(fd,&reads))FD_SET(fd,r);
  if(FD_ISSET(fd,&writes))FD_SET(fd,w);
 }
 return 1;
}
int ph_accept(int fd,struct sockaddr*a,socklen_t*n){
 (void)a;(void)n;assert(fd==3);stale();accept_calls++;
 if((mode==15&&next_child==5)||(mode==18&&accept_calls<=2))return -1;
 assert(next_child<=5);return next_child++;
}
ssize_t ph_recv(int fd,void*p,size_t n,int f){
 (void)f;assert(fd>=4&&fd<=5);stale();read_calls++;
 if(fd==4&&mode==11&&read_calls<=2)return -1; /* kernel EAGAIN */
 const char *wire=fd==4&&mode==12?post:get;
 size_t remaining=strlen(wire)-position[fd];
 if(!remaining)return -1; /* permanent kernel reset after POST header */
 if(n>remaining)n=remaining;
 memcpy(p,wire+position[fd],n);position[fd]+=n;return (ssize_t)n;
}
ssize_t ph_send(int fd,const void*p,size_t n,int f){
 (void)p;(void)f;assert(fd>=4&&fd<=5);stale();write_calls++;
 if(fd==4&&((mode==13&&write_calls<=2)||mode==14))return -1;
 output_length[fd]+=n;return (ssize_t)n;
}
int ph_close(int fd){
 assert(fd>=3&&fd<=5);stale();closed++;
 if(fd>3){assert(!client_closed[fd]);client_closed[fd]=1;}
 return mode==9?-1:0;
}
static void (*pending_thread)(mico_thread_arg_t);
static mico_thread_arg_t pending_arg;
int mico_rtos_create_thread(void*a,int p,const char*n,void(*fn)(mico_thread_arg_t),unsigned stack,mico_thread_arg_t arg){(void)p;(void)n;assert(a&&stack==12288);if(mode==10)return -7;*(mico_thread_t*)a=1;pending_thread=fn;pending_arg=arg;return 0;}
/* The production creator publishes the returned handle before the scheduled
 * owner runs. Execute deterministically after that publication in this fake. */
static int start_then_run(const ph_http_app *application,unsigned cap){
 int rc=ph_http_start(application,cap);
 if(!rc){assert(pending_thread);void(*fn)(mico_thread_arg_t)=pending_thread;pending_thread=NULL;fn(pending_arg);}
 return rc;
}
#define ph_http_start start_then_run
void mico_rtos_delete_thread(void*p){assert(!p);deleted++;}
void mico_thread_msleep(unsigned n){assert(n==100||n==1);tick+=n;if(n==1)retry_yields++;}
uint32_t mico_rtos_get_time(void){return tick;}
static ph_http_action begin(ph_http_exchange*x){
 assert(mode>=11);begins++;x->response.status=200;x->response.body="ok";x->response.length=2;
 if(!strcmp(x->request->path,"/body")){x->body_idle_ms=200;x->body_total_ms=400;return PH_HTTP_READ_BODY;}
 return PH_HTTP_REPLY;
}
static bool body(ph_http_exchange*x,const void*p,size_t n){(void)x;(void)p;(void)n;assert(0);return false;}
static void release(ph_http_exchange*x,bool sent){
 assert(x->request);releases++;if(!sent)failed++;
 else if((mode==15&&releases==1)||(mode!=15&&releases==2))delivered++;
}
static unsigned admission(void){return mode==17&&begins?2:3;}
static const ph_http_app app={begin,begin,body,NULL,release,admission};
static void reset(int selected){
 mode=selected;opened=closed=deleted=configured=nonblocks=getopts=0;
 tick=select_calls=accept_calls=read_calls=write_calls=0;
 begins=releases=delivered=failed=retry_yields=0;next_child=4;
 memset(client_closed,0,sizeof(client_closed));memset(position,0,sizeof(position));memset(output_length,0,sizeof(output_length));
}
int main(void){
 const int stale_values[]={EAGAIN,ERANGE,0};
 for(unsigned k=0;k<3;k++){
  poison=stale_values[k];
  for(int m=0;m<=18;m++){
   if(m==9)continue; /* uncertain-close ownership is latched: test last */
   reset(m);int rc=ph_http_start(&app,3);assert(ph_http_stop()==0);
   ph_http_diagnostics d=ph_http_snapshot();
   if(mode==10){assert(rc==-7&&!opened&&!deleted&&d.owner_state==2);continue;}
   assert(rc==0&&deleted==1&&opened==3);assert(closed==(mode==1?0:opened)+(mode==15?1:mode>=11?2:0));
   assert(d.owner_state==2&&!d.close_failures);
   if(mode==17)assert(configured==6&&getopts==12&&d.limit==2);
   assert(d.last_errno==0&&d.read_errno==0&&d.accept_errno==0&&d.last_close_errno==0&&d.set_errno==0);
   if(mode>=11){
    assert(delivered==1&&begins==releases&&retry_yields>0);
    if(mode==12||mode==14)assert(failed==1&&releases==2&&output_length[5]>0);
    else if(mode==15)assert(releases==1&&tick>=250&&accept_calls>3&&output_length[4]>0);
    else assert(!failed&&releases==2&&output_length[4]>0&&output_length[5]>0);
   }
  }
 }
 reset(9);assert(ph_http_start(&app,3)==0);assert(ph_http_stop()==0);
 ph_http_diagnostics d=ph_http_snapshot();assert(opened==1&&closed==1&&d.owner_state==3&&d.close_failures==1&&d.last_close_errno==0);
 assert(ph_http_start(&app,3)<0&&ph_http_snapshot().owner_state==3);
 return 0;
}

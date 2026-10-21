#include "mico.h"
#include "http.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>
static int mode,opened,closed,deleted,configured,nonblocks,cap=3;
static uint32_t tick;
int ph_socket(int a,int b,int c){(void)a;(void)b;(void)c;opened++;if(mode==1){errno=EMFILE;return -1;}return 3;}
int ph_bind(int fd,const struct sockaddr*a,socklen_t n){(void)a;(void)n;assert(fd==3);if(mode==2){errno=EIO;return -1;}return 0;}
int ph_listen(int fd,int n){assert(fd==3&&n==0);return mode==3?-1:0;}
int ph_setsockopt(int fd,int level,int option,const void*p,socklen_t n){assert(fd==3&&level==IPPROTO_TCP&&option==TCP_MAX_CONN_NUM&&n==sizeof(int));configured++;cap=*(const int*)p;return mode==4?-1:0;}
int ph_getsockopt(int fd,int level,int option,void*p,socklen_t*n){assert(fd==3&&level==IPPROTO_TCP);if(mode==5)return -1;*(int*)p=option==TCP_CONN_NUM?0:mode==6?cap+1:cap;*n=sizeof(int);return 0;}
int ph_fcntl(int fd,int cmd,...){assert(fd==3);va_list a;va_start(a,cmd);int flags=va_arg(a,int);va_end(a);if(mode==7)return -1;if(cmd==F_SETFL){assert(flags&O_NONBLOCK);nonblocks++;return 0;}return mode==8?0:O_NONBLOCK;}
int ph_select(int n,fd_set*r,fd_set*w,fd_set*e,struct timeval*t){(void)r;(void)w;(void)e;assert(n==64&&t->tv_usec==25000);errno=EBADF;return -1;}
int ph_accept(int fd,struct sockaddr*a,socklen_t*n){(void)fd;(void)a;(void)n;assert(0);return -1;}
ssize_t ph_recv(int fd,void*p,size_t n,int f){(void)fd;(void)p;(void)n;(void)f;assert(0);return -1;}
ssize_t ph_send(int fd,const void*p,size_t n,int f){(void)fd;(void)p;(void)n;(void)f;assert(0);return -1;}
int ph_close(int fd){assert(fd==3);closed++;if(mode==9){errno=EIO;return -1;}return 0;}
int mico_rtos_create_thread(void*a,int p,const char*n,void(*fn)(mico_thread_arg_t),unsigned stack,mico_thread_arg_t arg){(void)a;(void)p;(void)n;assert(stack==12288);if(mode==10)return -7;fn(arg);return 0;}
void mico_rtos_delete_thread(void*p){assert(!p);deleted++;}
void mico_thread_msleep(unsigned n){assert(n==100||n==1);tick+=n;}
uint32_t mico_rtos_get_time(void){return tick;}
static ph_http_action begin(ph_http_exchange*x){(void)x;assert(0);return PH_HTTP_REPLY;}
static void release(ph_http_exchange*x,bool sent){(void)x;(void)sent;assert(0);}
static const ph_http_app app={begin,begin,NULL,NULL,release,NULL};
int main(void){for(int index=0;index<=10;index++){mode=index==9?10:index==10?9:index;opened=closed=deleted=configured=nonblocks=0;int rc=ph_http_start(&app,3,false);assert(ph_http_stop()==0);if(mode==10){assert(rc==-7&&!opened&&!deleted&&ph_http_snapshot().owner_state==2);continue;}assert(rc==0&&deleted==1);assert(opened==(mode==9?1:3));assert(closed==(mode==1?0:opened));ph_http_diagnostics d=ph_http_snapshot();assert(d.owner_state==(mode==9?3:2));if(mode==9)assert(d.close_failures==1&&d.last_close_errno==EIO);else assert(!d.close_failures);if(mode<2)assert(configured==(mode==1?0:3));}assert(ph_http_start(&app,3,false)<0);assert(ph_http_snapshot().owner_state==3);return 0;}

/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "os.h"
#include "port.h"
#include "boot_trace.h"
#include "../protocol/httpd/http.h"
#include <errno.h>
#include <string.h>
#ifdef PH_HOST_TEST
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/ioctl.h>
#else
#include <lwip/sockets.h>
#endif
#ifndef PH_HTTP_PORT
#define PH_HTTP_PORT 80
#endif
static ph_http_server server;
static ph_http_diagnostics cached;
static ph_os_mutex diagnostics_mutex;
static volatile bool started;
extern const ph_http_app bl_http_app;
static int listener(unsigned cap) {
 int fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(fd<0)return -1;
 int reuse=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
 struct sockaddr_in a;memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons(PH_HTTP_PORT);a.sin_addr.s_addr=INADDR_ANY;
 if(bind(fd,(struct sockaddr*)&a,sizeof(a))<0||listen(fd,(int)cap)<0){close(fd);return -1;}return fd;
}
static int limit(int fd,unsigned cap,ph_http_diagnostics *d) {
 /* lwIP's TCP backlog and this core's four slots bound admissions. There is no
  * MiCO TCP_MAX_CONN_NUM socket option in this SDK. */
 (void)fd;d->limit=(int)cap;d->set_rc=d->limit_get_rc=0;return 0;
}
static int nonblock(int fd) {unsigned long value=1;return ioctl(fd,FIONBIO,&value);}
static int accept_one(int fd) {return accept(fd,NULL,NULL);}
static int wait_ready(uint64_t reads,uint64_t writes,uint64_t *rr,uint64_t *ww,unsigned ms) {
 fd_set r,w;FD_ZERO(&r);FD_ZERO(&w);int high=-1;
 for(int fd=0;fd<PH_HTTP_FD_LIMIT;++fd){if(reads&(UINT64_C(1)<<fd)){FD_SET(fd,&r);high=fd;}if(writes&(UINT64_C(1)<<fd)){FD_SET(fd,&w);if(fd>high)high=fd;}}
 struct timeval tv={ms/1000,(ms%1000)*1000};int rc=select(high+1,&r,&w,NULL,&tv);*rr=*ww=0;
 if(rc>0)for(int fd=0;fd<=high;++fd){if(FD_ISSET(fd,&r))*rr|=UINT64_C(1)<<fd;if(FD_ISSET(fd,&w))*ww|=UINT64_C(1)<<fd;}
 return rc;
}
static int receive(int fd,void *p,size_t n){return recv(fd,p,n,0);}
static int transmit(int fd,const void *p,size_t n){return send(fd,p,n,0);}
static int close_one(int fd){return close(fd);}
static bool retry(void){return errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK;}
static int error_number(void){return errno;}
static void yield(void){ph_os_sleep(1);}
static void publish(const ph_http_diagnostics *d){ph_os_lock(&diagnostics_mutex);cached=*d;ph_os_unlock(&diagnostics_mutex);}
ph_http_diagnostics ph_http_snapshot(void){ph_http_diagnostics d={0};if(started){ph_os_lock(&diagnostics_mutex);d=cached;ph_os_unlock(&diagnostics_mutex);}return d;}
const ph_http_io bl_http_io={ph_os_now,listener,limit,accept_one,nonblock,wait_ready,receive,transmit,close_one,retry,error_number,yield,publish};
static void owner(beken_thread_arg_t arg) {
 (void)arg;
 PH_BOOT_STAGE("http-thread");
 for(unsigned attempt=0;attempt<3;++attempt) {
  if(ph_http_server_open(&server,&bl_http_io,&bl_http_app,PH_HTTP_CLIENT_SLOTS)) {
   PH_BOOT_STAGE("http-listening");
   while(server.running)ph_http_server_step(&server);
   ph_http_server_close(&server);
  }
  if(server.diagnostics.close_failures)break;
  ph_os_sleep(100);
 }
 rtos_delete_thread(NULL);
}
void PH_BL_StartHTTP(void) {
 if(started||ph_os_mutex_init(&diagnostics_mutex))return;
 started=true;
 int result=rtos_create_thread(NULL,BEKEN_APPLICATION_PRIORITY,"openiot-http",owner,PH_HTTP_STACK_BYTES,0);
 PH_BOOT_RESULT("http-created",result);
 if(result)started=false;
}

"""Execute the patched SDK's real client loop across recoverable socket errors."""
import os
from pathlib import Path
import unittest
from tests import test_memory_admission as admission
from tests.test_tc1_sdk_migration import function
ROOT = Path(__file__).resolve().parents[1]
class Recovery(unittest.TestCase):
 def test_tcp_snapshot_preserves_unknown_values_on_option_errors(self):
  source=admission.MemoryAdmission().sdk_http_source('httpd.c')
  admission.MemoryAdmission().run_c(r'''
#include <assert.h>
#include <sys/socket.h>
#include <netinet/in.h>
#define TCP_MAX_CONN_NUM 7
#define TCP_CONN_NUM 6
static int http_sockfd=3,mode;
static int fake_getsockopt(int fd,int level,int option,void *v,socklen_t *n){
 assert(fd==3&&level==IPPROTO_TCP&&*n==sizeof(int));
 if(mode==option)return -1;
 *(int*)v=option==7?3:2;if(mode==1)*n=0;return 0;
}
#define getsockopt fake_getsockopt
SNAPSHOT
int main(void){int limit,connections;
 assert(!phicomm_http_tcp_snapshot(&limit,&connections)&&limit==3&&connections==2);
 mode=7;assert(phicomm_http_tcp_snapshot(&limit,&connections)<0&&limit==-1&&connections==-1);
 mode=6;assert(phicomm_http_tcp_snapshot(&limit,&connections)<0&&limit==3&&connections==-1);
 mode=1;assert(phicomm_http_tcp_snapshot(&limit,&connections)<0&&limit==-1&&connections==-1);
}
'''.replace('SNAPSHOT',function(source,'int phicomm_http_tcp_snapshot(')))

 def test_accepted_socket_is_nonblocking_or_closed(self):
  source=admission.MemoryAdmission().sdk_http_source('httpd.c')
  admission.MemoryAdmission().run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#define kNoErr 0
#define kInProgressErr 1
#define FALSE false
#define httpd_d(...) ((void)0)
static int http_sockfd=3,client_sockfd=-1,fcntl_failure,closes,configured;
static bool https_active;
static int fake_accept(int fd,struct sockaddr *addr,socklen_t *n){assert(fd==3&&addr&&n);return 7;}
static int fake_fcntl(int fd,int op,int value){assert(fd==7&&op==F_SETFL&&value==O_NONBLOCK);configured++;return fcntl_failure?-1:0;}
static int fake_close(int fd){assert(fd==7);closes++;return 0;}
static int fake_setsockopt(int fd,int level,int key,const void *p,socklen_t n){(void)level;(void)key;assert(fd==7&&p&&n==sizeof(int));return 0;}
#define accept fake_accept
#define fcntl fake_fcntl
#define close fake_close
#define setsockopt fake_setsockopt
ACCEPT
int main(void){fd_set fds;FD_ZERO(&fds);FD_SET(3,&fds);
 fcntl_failure=1;assert(httpd_accept_client_socket(&fds)<0&&client_sockfd==-1&&closes==1&&configured==1);
 fcntl_failure=0;assert(!httpd_accept_client_socket(&fds)&&client_sockfd==7&&closes==1&&configured==2);
}
'''.replace('ACCEPT',function(source,'static int httpd_accept_client_socket(')))

 def test_readiness_race_retries_and_io_deadline_survives_tick_wrap(self):
  source=admission.MemoryAdmission().sdk_http_source('httpd_handle.c')
  admission.MemoryAdmission().run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <sys/select.h>
#define kNoErr 0
#define kInProgressErr 1
static uint32_t now;static int calls,mode;
static uint32_t mico_rtos_get_time(void){return now;}
static void mico_thread_msleep(unsigned ms){assert(ms==1);now+=ms;}
static int fake_select(int n,fd_set *r,fd_set *w,fd_set *e,struct timeval *t){assert(n==8&&!!r!=!!w&&!e&&t->tv_usec==200000);now+=200;return mode==4?-1:mode==3?0:1;}
static int fake_send(int fd,const void *p,size_t n,int f){assert(fd==7&&p&&n&&!f);calls++;
 if(mode==1||calls==1){errno=EAGAIN;return -1;}if(mode==2){errno=ECONNRESET;return -1;}return n>2?2:(int)n;}
static int fake_recv(int fd,void *p,size_t n,int f){assert(fd==7&&p&&n&&!f);calls++;
 if(mode==1||calls==1){errno=EWOULDBLOCK;return -1;}if(mode==2){errno=ECONNRESET;return -2;}return mode==5?0:1;}
#define select fake_select
#define send fake_send
#define recv fake_recv
SEND
RECV
int main(void){char b[8];
 for(mode=0;mode<=5;mode++){
  now=UINT32_MAX-99;calls=0;int rc=httpd_send(7,"12345",5);
  assert((mode==0||mode==5)?rc==0:rc<0);assert((uint32_t)(now-(UINT32_MAX-99))<=5201);
  now=UINT32_MAX-99;calls=0;rc=httpd_recv(7,b,sizeof(b),0);
  assert(mode==0?rc==1:mode==5?rc==0:rc==-1);assert((uint32_t)(now-(UINT32_MAX-99))<=5201);
 }
}
'''.replace('SEND',function(source,'int httpd_send(')).replace('RECV',function(source,'int httpd_recv(')))

 def test_reset_select_and_close_do_not_suspend_listener(self):
  helper=admission.MemoryAdmission();source=helper.sdk_http_source('httpd.c')
  prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <sys/select.h>
#define FALSE false
#define HTTPD_TIMEOUT_EVENT 0
#define HTTPD_CLIENT_SOCK_TIMEOUT 2
#define HTTPD_DONE 1
#define kNoErr 0
#define httpd_d(...) ((void)0)
static bool httpd_stop_req;
static int client_sockfd=-1,select_failure,close_failure,closes,handled,selects;
static void httpd_suspend_thread(bool fault){(void)fault;assert(!"HTTP service suspended");}
static int net_get_sock_error(int fd){(void)fd;return -1;}
static int fake_select(int n,fd_set *r,fd_set *w,fd_set *e,struct timeval *t){
 (void)n;(void)r;(void)w;(void)e;(void)t;selects++;return select_failure?-1:1;
}
#define select fake_select
SELECT
static int httpd_accept_client_socket(const fd_set *fds){assert(fds);client_sockfd=7;return 0;}
static int fake_close(int fd){assert(fd==7);closes++;return close_failure?-1:0;}
#define close fake_close
static int httpd_handle_message(int fd){assert(fd==7);handled++;return HTTPD_DONE;}
CLIENT
int main(void){
 fd_set fds;FD_ZERO(&fds);FD_SET(3,&fds);
 select_failure=close_failure=1;httpd_handle_client_connection(&fds);
 assert(client_sockfd==-1 && closes==1 && handled==0 && selects==1);
 select_failure=0;httpd_handle_client_connection(&fds);
 assert(client_sockfd==-1 && closes==2 && handled==1 && selects==2);
 close_failure=0;httpd_handle_client_connection(&fds);
 assert(client_sockfd==-1 && closes==3 && handled==2 && selects==3);
}
'''
  helper.run_c(prefix.replace('SELECT\n','\n'+function(source,'static int httpd_select(')).replace('\nCLIENT\n','\n'+function(source,'static void httpd_handle_client_connection(')))

 def test_purge_stops_on_eof_and_error(self):
  helper=admission.MemoryAdmission();source=helper.sdk_http_source('httpd_handle.c')
  helper.run_c(r'''
#include <assert.h>
#define kNoErr 0
#define httpd_d(...) ((void)0)
#pragma GCC diagnostic ignored "-Wsign-compare"
typedef struct {unsigned body_nbytes;} httpd_request_t;
static int calls,read_result;
static int httpd_parse_hdr_tags(httpd_request_t *r,int fd,char *b,int n){assert(fd==7&&b&&n==16);r->body_nbytes=8;return 0;}
static int httpd_recv(int fd,char *b,unsigned n,int flags){assert(fd==7&&b&&n<=8&&!flags&&++calls<4);return read_result;}
PURGE
int main(void){
 httpd_request_t r;char scratch[16];
 for(read_result=-1;read_result<=0;read_result++){calls=0;httpd_purge_socket_data(&r,scratch,16,7);assert(calls==1);}
 read_result=4;calls=0;httpd_purge_socket_data(&r,scratch,16,7);assert(calls==2);
}
'''.replace('PURGE',function(source,'void httpd_purge_socket_data(')))

 def test_incomplete_command_frees_body_and_closes_without_dispatch_or_send(self):
  source=(ROOT/'TC1/http_server/app_httpd.c').read_text()
  admission.MemoryAdmission().run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define kParamErr -2
#define HTTPD_DONE 3
typedef int json_object;
typedef struct {int chunked,remaining_bytes;} httpd_request_t;
static int read_error,released,freed,dispatched,replied,parsed,put;
static char storage[64];static json_object object;
static int read_headers(httpd_request_t *r){(void)r;return 0;}
static bool ota_job_active(void){return false;}
static unsigned service_memory_wait(int n){assert(n==16);return 528;}
static void service_memory_release(unsigned n){assert(n==528);released++;}
static int HttpV1CommandBusy(httpd_request_t *r){(void)r;assert(0);return -1;}
static uint64_t service_memory_failures(void){return 0;}
static char *json_heap_malloc(size_t n){assert(n==17);return storage;}
static void json_heap_free(void *p){assert(p==storage);freed++;}
static int read_body(httpd_request_t *r,char *p,int n){(void)r;assert(p==storage&&n==17);strcpy(p,"{}");return read_error;}
static json_object *service_parse(const char *p,size_t n){assert(p==storage&&n==2);parsed++;return &object;}
static json_object *service_command(json_object *p){assert(p==&object);dispatched++;return &object;}
static void json_object_put(json_object *p){assert(p==&object);put++;}
static int HttpV1Reply(httpd_request_t *r,json_object *p){(void)r;assert(p==&object);replied++;return 0;}
COMMAND
int main(void){
 httpd_request_t r={0,16};read_error=-1;
 assert(HttpV1Command(&r)==HTTPD_DONE && freed==1 && released==1);
 assert(!parsed&&!dispatched&&!replied&&!put);
 read_error=0;assert(!HttpV1Command(&r)&&freed==2&&released==2);
 assert(parsed==1&&dispatched==1&&replied==1&&put==1);
}
'''.replace('COMMAND',function(source,'static int HttpV1Command(')))
if __name__=='__main__':unittest.main()

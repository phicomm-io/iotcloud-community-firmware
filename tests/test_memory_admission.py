"""Run actual no-heap admission/wait/HTTP paths with a changing heap budget."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from tests.test_tc1_sdk_migration import function
ROOT = Path(__file__).resolve().parents[1]

class MemoryAdmission(unittest.TestCase):
    def sdk_http_source(self, filename):
        sdk = Path(os.environ.get('TC1_SDK_DIR', ROOT / 'build/tc1-sdk-3.6.4/mico-os'))
        path = sdk / 'libraries/daemons/http_server' / filename
        if not path.is_file():
            self.skipTest('build TC1 first to test the applied SDK overlay')
        return path.read_text()

    def run_c(self, source):
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory)/'test.c'; p.write_text(source)
            cmd=[os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(ROOT),str(p),'-o',str(p.with_suffix(''))]
            if os.environ.get('SANITIZE')=='1': cmd+=['-g','-fno-omit-frame-pointer','-fsanitize=address,undefined']
            subprocess.run(cmd,check=True);subprocess.run([str(p.with_suffix(''))],check=True)

    def test_budget_overlap_boundaries_and_wait_recovery(self):
        self.run_c(r'''
#include <assert.h>
#include <stdio.h>
#include "common/memory_budget.h"
static service_memory_state_t state;
static uint32_t now,free_bytes,recover_at;
static unsigned sleeps;
static unsigned service_memory_claim(size_t n){return memory_budget_claim(&state,free_bytes,n);}
static void service_memory_waiting(bool enter){if(enter)state.waiting++;else{assert(state.waiting);state.waiting--;}}
static uint32_t mico_rtos_get_time(void){return now;}
static void mico_thread_msleep(unsigned ms){assert(ms==25 && state.waiting==1);sleeps++;now+=ms;if(sleeps==recover_at)free_bytes=40000;}
#include "common/memory_wait.h"
int main(void){
 state.minimum_free=UINT32_MAX;
 assert(!memory_budget_claim(&state,0,0));assert(!memory_budget_claim(&state,16895,0));
 unsigned a=memory_budget_claim(&state,16896,0);assert(a==512 && state.active==1);
 assert(!memory_budget_claim(&state,17407,0)); /* competing builders share the budget */
 unsigned b=memory_budget_claim(&state,17408,0);assert(b && state.active==2);
 memory_budget_release(&state,a);memory_budget_release(&state,b);assert(!state.active && !state.reserved);
 assert(!memory_budget_claim(&state,UINT32_MAX,SIZE_MAX));
 a=memory_budget_claim(&state,25088,8192);assert(a==8704);memory_budget_release(&state,a);
 now=UINT32_MAX-50;free_bytes=1000;recover_at=4;
 a=service_memory_wait(0);assert(a && sleeps==4 && now==49 && !state.waiting);memory_budget_release(&state,a);
 sleeps=0;recover_at=UINT32_MAX;free_bytes=1000;now=UINT32_MAX-50;
 assert(!service_memory_wait(0) && sleeps==80 && !state.waiting);
 sleeps=0;assert(!service_memory_wait(1025) && !sleeps && !state.waiting);
 assert(!service_memory_wait(8192) && !sleeps && !state.waiting);
 free_bytes=25088;a=service_memory_wait(8192);assert(a==8704 && !sleeps);memory_budget_release(&state,a);
 free_bytes=1000;assert(!service_memory_wait(1024) && sleeps==80 && !state.waiting);
 assert(!state.active && !state.reserved && state.minimum_free==0);
 puts("Shared reservations: floor/overlap/overflow; wait keeps descriptor, resumes, times out across tick wrap: OK");
}
''')

    def test_memory_diagnostics_fit_even_at_maximum_counters(self):
        for model in ('A1','TC1'):
            path='A1/http.c' if model=='A1' else 'TC1/http_server/app_httpd.c'
            source=(ROOT/path).read_text()
            signature='static void memory_response(' if model=='A1' else 'static int HttpV1Memory('
            harness=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "common/memory_budget.h"
typedef struct {int free_memory,allocted_memory,num_of_chunks;} micoMemInfo_t;
static const micoMemInfo_t *MicoGetMemoryInfo(void){static micoMemInfo_t m={2147483647,2147483647,2147483647};return &m;}
static void service_memory_snapshot(service_memory_state_t *b){memset(b,0xff,sizeof(*b));}
static uint32_t mico_rtos_get_time(void){return UINT32_MAX;}
static unsigned sent;
static void check(const char *body,size_t n){assert(n<896 && n==strlen(body));assert(strstr(body,"\"mqtt_overflow\":4294967295"));assert(strstr(body,"\"mqtt_max_delay_ms\":4294967295"));sent++;}
'''
            if model=='A1':
                harness+=r'''
static void response(int fd,unsigned code,const char *type,const void *body,size_t n,bool gzip){assert(fd==7 && code==200 && !strcmp(type,"application/json") && !gzip);check(body,n);}
'''+function(source,signature)+'\nint main(void){memory_response(7);assert(sent==1);}'
            else:
                harness+=r'''
int get_last_reset_reason(void){return 4;}
int phicomm_http_tcp_snapshot(int *limit,int *connections){*limit=2147483647;*connections=2147483647;return -2147483647;}
typedef int httpd_request_t;
#define HTTP_RES_200 "200"
static int response_json(httpd_request_t *r,const char *status,const char *body,size_t n){assert(*r==7 && !strcmp(status,"200"));check(body,n);return 0;}
static int HttpV1Busy(httpd_request_t *r){(void)r;assert(0);return -1;}
'''+function(source,signature)+'\nint main(void){httpd_request_t r=7;assert(!HttpV1Memory(&r) && sent==1);}'
            self.run_c(harness)

    def test_fixed_busy_identifiers(self):
        self.run_c(r'''
#include <assert.h>
#include <stdio.h>
#include "common/busy_reply.h"
static void check(const char *s,const char *id){char out[SERVICE_BUSY_BYTES];service_busy_wire(s,s?strlen(s):0,out);assert(strstr(out,"\"code\":\"busy\""));if(id)assert(strstr(out,id));else assert(!strstr(out,"request_id"));}
int main(void){
 check(NULL,NULL);check("",NULL);check("{}",NULL);
 check("{\"x\":[{\"request_id\":\"nested\"}],\"request_id\":\"real-1\"}","real-1");
 check("{\"request_id\":\"escaped\\u002d1\"}","escaped-1");
 check("{\"request_id\":null}",NULL);check("{\"request_id\":\"bad\\u0000tail\"}",NULL);
 check("{\"request_id\":\"bad\\\"tail\"}",NULL);check("{\"request_id\":\"valid\"}trailing",NULL);
 check("{\"request_id\":\"valid\",",NULL);
 char p[512];snprintf(p,sizeof(p),"{\"request_id\":\"%080d\"}",1);check(p,NULL);
 /* Truncation and arbitrary escaped bytes cannot overflow fixed buffers. */
 for(unsigned c=1;c<256;c++){snprintf(p,sizeof(p),"{\"request_id\":\"\\%c\"}",c);check(p,NULL);}
 puts("Fixed overload replies: top-level request ID, escaping, truncation and oversized ID bounded: OK");
}
''')

    def test_tc1_listener_configured_once_before_listen_and_fails_closed(self):
        source = self.sdk_http_source('httpd.c')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <setjmp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#define kNoErr 0
#define kInProgressErr 12
#define TCP_MAX_CONN_NUM 7
#define UNUSED_PARAMETER(x) (void)(x)
#define httpd_d(...) ((void)0)
typedef unsigned mico_thread_arg_t;
static int http_sockfd, configured, listened, closed, fail_option, cycles;
static jmp_buf done;
static int phicomm_http_client_limit(void){return 2;}
static int net_get_sock_error(int fd){(void)fd;return -12;}
#define socket fake_socket
#define bind fake_bind
#define listen fake_listen
#define setsockopt fake_setsockopt
#define close fake_close
static int socket(int a,int b,int c){assert(a==AF_INET&&b==SOCK_STREAM&&c==IPPROTO_TCP);return 3;}
static int bind(int fd,const struct sockaddr *a,socklen_t n){assert(fd==3&&a&&n==sizeof(struct sockaddr_in));return 0;}
static int setsockopt(int fd,int level,int name,const void *v,socklen_t n){
 assert(fd==3&&n==sizeof(int));if(level==IPPROTO_TCP){assert(name==TCP_MAX_CONN_NUM&&*(const int*)v==2&&!listened);configured++;return fail_option?-1:0;}return 0;
}
static int listen(int fd,int n){assert(fd==3&&n==2&&configured==1);listened++;return 0;}
static int close(int fd){assert(fd==3);closed++;return 0;}
SETUP
static int httpd_setup_main_sockets(void){http_sockfd=httpd_setup_new_socket(80);return http_sockfd<0?-12:0;}
static void httpd_suspend_thread(bool b){(void)b;assert(0);}
static void mico_thread_msleep(unsigned n){assert(n==20);}
static int httpd_select(int max,fd_set *r,fd_set *a,int timeout){assert(max==3&&r&&a&&timeout==-1);if(++cycles==4)longjmp(done,1);return 1;}
static void httpd_handle_client_connection(fd_set *s){assert(s&&configured==1&&listened==1);}
MAIN
int main(void){
 if(!setjmp(done))httpd_main(0);
 assert(configured==1&&listened==1&&cycles==4&&!closed);
 configured=listened=0;fail_option=1;assert(httpd_setup_new_socket(80)<0);assert(configured==1&&!listened&&closed==1);
}
'''.replace('SETUP', function(source, 'static int httpd_setup_new_socket(')).replace('MAIN', function(source, 'static void httpd_main(')))

    def test_http_retains_fd_until_recovery_and_never_executes_timeout(self):
        for model in ('A1','M1'):
            source=(ROOT/model/'http.c').read_text()
            helpers=source[source.index('static void memory_busy(int fd)\n'):source.index('static void light_execute(') if model=='A1' else source.index('/* Keep accept responsive')]
            self.run_c(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "common/memory_budget.h"
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#define MODEL_JSON_LIMIT 8192
static uint32_t now;static unsigned attempts,recover_at,waiting,active,reads,queries,commands,sends,status;
typedef int json_object;
static uint64_t service_memory_failures(void){return 0;}
static unsigned service_memory_claim(size_t n){assert(n<=8192);if(++attempts<recover_at)return 0;active++;return 12288;}
static void service_memory_release(unsigned n){assert(n==12288 && active==1);active--;}
static void service_memory_waiting(bool enter){if(enter)waiting++;else waiting--;}
static uint32_t mico_rtos_get_time(void){return now;}
static void mico_thread_msleep(unsigned n){assert(n==25 && waiting==1 && !reads && !queries && !commands && !sends);now+=n;}
#include "common/memory_wait.h"
static void response(int fd,int code,const char *type,const void *p,size_t n,bool gzip){assert(fd==7 && code==503 && !gzip && !strcmp(type,"application/json"));assert(n==15 && !memcmp(p,"{\"code\":\"busy\"}",n));sends++;status=code;}
static json_object *service_query(const char *a){assert(active==1 && !strcmp(a,"status.get"));queries++;return (void*)1;}
static json_object *read_json(int fd,uint32_t n){assert(fd==7 && n==100 && active==1);reads++;return (void*)1;}
static json_object *service_command(json_object *q){assert(q && active==1);commands++;return (void*)2;}
static void json_object_put(json_object *q){assert(q==(void*)1);}
static void send_json(int fd,json_object *q){assert(fd==7 && q && active==1);sends++;status=200;}
HELPERS
static void reset(unsigned after){now=attempts=waiting=active=reads=queries=commands=sends=status=0;recover_at=after;}
int main(void){
 reset(5);command_response(7,100);assert(now==100 && reads==1 && commands==1 && status==200 && !active && !waiting);
 reset(5);query_response(7,"status.get");assert(now==100 && queries==1 && !reads && status==200 && !active);
 reset(1000);command_response(7,100);assert(now==2000 && !reads && !commands && status==503 && !active && !waiting);
 reset(1000);command_response(7,8192);assert(!now && !reads && !commands && status==503 && !active && !waiting);
 reset(1);query_response(7,"status.get");assert(!now && queries==1 && status==200 && !active);
 puts("MODEL HTTP actual helpers: no body/query allocation while waiting, same FD resumes; timeout no execution; release: OK");
}
'''.replace('MODEL',model).replace('HELPERS',helpers))

    def test_tc1_options_full_request_line(self):
        source=self.sdk_http_source('http_parse.c')
        constants='\n'.join('#define HTTPD_REQ_TYPE_'+name+' '+str(i) for i,name in enumerate(('GET','POST','HEAD','PUT','DELETE','OPTIONS'),1))
        # This function sees the whole first line, rather than a NUL-terminated method.
        self.run_c('#include <assert.h>\n#include <string.h>\n#define HTTPD_REQ_TYPE_UNKNOWN 0\n#define kNoErr 0\n#define kInProgressErr 1\n'+constants+r'''
static const char http_post[]="POST",http_get[]="GET",http_head[]="HEAD",http_put[]="PUT",http_delete[]="DELETE";
'''+function(source,'static int httpd_verify_req_type(')+r'''
int main(void){int type=0;assert(!httpd_verify_req_type("OPTIONS /api/v1/info HTTP/1.1",&type));assert(type==HTTPD_REQ_TYPE_OPTIONS);assert(httpd_verify_req_type("OPTIONSX / HTTP/1.1",&type)<0);}
''')

    def test_rejected_posts_deliver_busy_without_reset(self):
        for product in ('TC1','A1','M1'):
            source=(ROOT/('TC1/http_server/app_httpd.c' if product=='TC1' else product+'/http.c')).read_text()
            helper=function(source,'static int HttpV1CommandBusy(') if product=='TC1' else 'static void memory_busy(int fd){httpd_request_t r={fd,0};assert(!HttpV1Busy(&r));}\n'+function(source,'static void memory_busy_body(')
            invoke='assert(!HttpV1CommandBusy(&r));assert(r.remaining_bytes==(full?0:8192));' if product=='TC1' else 'memory_busy_body(r.sock,8192);'
            self.run_c(r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <time.h>
typedef struct {int sock,remaining_bytes;} httpd_request_t;
static uint32_t mico_rtos_get_time(void){struct timespec ts;assert(!clock_gettime(CLOCK_MONOTONIC,&ts));return (uint32_t)(ts.tv_sec*1000u+ts.tv_nsec/1000000u);}
static int HttpV1Busy(httpd_request_t *r){
 const char busy[]="HTTP/1.1 503 Service Unavailable\r\nContent-Length: 15\r\nConnection: close\r\n\r\n{\"code\":\"busy\"}";
 size_t n=0;while(n<sizeof(busy)-1){int rc=send(r->sock,busy+n,sizeof(busy)-1-n,0);assert(rc>0);n+=rc;}return 0;
}
'''+helper+r'''
int main(void){
 for(int full=0;full<=1;full++){
  int listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);
  struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
  assert(!bind(listener,(void*)&addr,sizeof(addr)) && !listen(listener,1));
  socklen_t size=sizeof(addr);assert(!getsockname(listener,(void*)&addr,&size));
  pid_t child=fork();assert(child>=0);
  if(!child){
   int fd=accept(listener,NULL,NULL);assert(fd>=0);close(listener);
   httpd_request_t r={fd,8192};uint32_t start=mico_rtos_get_time();
   INVOKE assert(mico_rtos_get_time()-start<500);close(fd);_exit(0);
  }
  close(listener);int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);
  assert(!connect(fd,(void*)&addr,sizeof(addr)));
  char body[8192];memset(body,' ',sizeof(body));
  if(full){size_t sent=0;while(sent<sizeof(body)){int rc=send(fd,body+sent,sizeof(body)-sent,0);assert(rc>0);sent+=rc;}}
  char out[256];int used=0,n;
  while((n=recv(fd,out+used,sizeof(out)-1-used,0))>0)used+=n;
  assert(n==0);out[used]=0;assert(strstr(out,"503 Service Unavailable") && strstr(out,"{\"code\":\"busy\"}"));
  close(fd);int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
 }
 puts("Actual TCP rejected POST: complete busy, no reset, no heap; omitted body bounded at 100 ms: OK");
}
'''.replace('INVOKE',invoke))

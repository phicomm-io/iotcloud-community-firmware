"""Execute A1/M1 listener ownership and response deadlines with SDK stubs."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#define socket fake_socket
#define bind fake_bind
#define listen fake_listen
#define setsockopt fake_setsockopt
#define getsockopt fake_getsockopt
#define accept fake_accept
#define close fake_close
#define select fake_select
#define send fake_send
#define recv fake_recv
#define fcntl fake_fcntl
'''

OWNER = PRELUDE + r'''
typedef int OSStatus;
typedef int mico_queue_t;
typedef uintptr_t mico_thread_arg_t;
#define kNoErr 0
#define MICO_APPLICATION_PRIORITY 7
#define MICO_WAIT_FOREVER UINT32_MAX
#define TCP_MAX_CONN_NUM 7
#define TCP_CONN_NUM 6
#define UNUSED_PARAMETER(x) (void)(x)
#define custom_log(...) ((void)0)
static int thread, a1_lock, fail_stage, deletes, deinit, listener_closes;
static int accept_calls, diag_reads, handled, client_closes, sleeps;
static int queued[8], queued_count, popped;
static void (*accept_entry)(mico_thread_arg_t), (*worker_entry)(mico_thread_arg_t);
static jmp_buf stop;
static bool ota_job_active(void) { return false; }
static void mico_rtos_lock_mutex(int *lock) { assert(!*lock); *lock=1; }
static void mico_rtos_unlock_mutex(int *lock) { assert(*lock); *lock=0; }
static uint32_t mico_rtos_get_time(void) { return 100; }
static void mico_thread_msleep(unsigned ms) { assert(ms==100); ++sleeps; }
static void mico_rtos_delete_thread(void *p) { assert(!p && thread==1); ++deletes; }
static int mico_rtos_create_thread(void *p,int priority,const char *name,
                                 void (*entry)(mico_thread_arg_t),unsigned stack,mico_thread_arg_t arg) {
    assert(!p && priority==7 && arg==0);
    if(thread==0) { assert(!strcmp(name,"a1_http_accept") && stack==2048); accept_entry=entry; }
    else { assert(thread==1 && !strcmp(name,"a1_http") && stack==8192); if(fail_stage==5)return -1; worker_entry=entry; }
    return 0;
}
static int socket(int domain,int type,int protocol) {
    assert(thread==1 && domain==AF_INET && type==SOCK_STREAM && protocol==IPPROTO_TCP);
    return fail_stage==1?-1:3;
}
static int bind(int fd,const struct sockaddr *addr,socklen_t n) {
    assert(thread==1 && fd==3 && n==sizeof(struct sockaddr_in));
    assert(((const struct sockaddr_in *)addr)->sin_port==htons(80));
    return fail_stage==2?-1:0;
}
static int listen(int fd,int backlog) { assert(thread==1 && fd==3 && backlog==0); return fail_stage==3?-1:0; }
static int setsockopt(int fd,int level,int option,const void *value,socklen_t n) {
    assert(thread==1 && fd==3 && level==IPPROTO_TCP && option==TCP_MAX_CONN_NUM);
    assert(n==sizeof(int) && *(const int *)value==24); return 0;
}
static int getsockopt(int fd,int level,int option,void *value,socklen_t *n) {
    assert(thread==1 && fd==3 && level==IPPROTO_TCP && *n==sizeof(int));
    assert(option==TCP_MAX_CONN_NUM || option==TCP_CONN_NUM);
    *(int *)value=option==TCP_MAX_CONN_NUM?24:2; ++diag_reads; return 0;
}
static int mico_rtos_init_queue(mico_queue_t *q,const char *name,unsigned size,unsigned count) {
    (void)q; (void)name; assert(thread==1 && size==sizeof(int) && count==8);
    return fail_stage==4?-1:0;
}
static void mico_rtos_deinit_queue(mico_queue_t *q) { (void)q; assert(thread==1); ++deinit; }
static int accept(int fd,struct sockaddr *addr,socklen_t *n) {
    assert(thread==1 && fd==3 && !addr && !n);
    if(accept_calls++==0) { errno=EAGAIN; return -1; }
    if(accept_calls>3)longjmp(stop,1);
    return accept_calls+5;
}
static int mico_rtos_push_to_queue(mico_queue_t *q,void *fd,uint32_t timeout) {
    (void)q; assert(thread==1 && timeout==0 && queued_count<8);
    queued[queued_count++]=*(int *)fd; return 0;
}
static int mico_rtos_pop_from_queue(mico_queue_t *q,void *fd,uint32_t timeout) {
    (void)q; assert(thread==2 && timeout==MICO_WAIT_FOREVER);
    if(popped==queued_count)longjmp(stop,1);
    *(int *)fd=queued[popped++]; return 0;
}
static void handle(int fd) { assert(thread==2 && (fd==7 || fd==8)); ++handled; }
static int close(int fd) {
    if(fd==3) { assert(thread==1); ++listener_closes; }
    else { assert(thread==2 && (fd==7 || fd==8)); ++client_closes; }
    return 0;
}
/* The unchanged queue-rejection branch is exercised by test_http_overload. */
static int fcntl(int fd,int command,int flags) { (void)fd; (void)command; (void)flags; assert(0); return -1; }
static int select(int n,fd_set *r,fd_set *w,fd_set *e,struct timeval *t) {
    (void)n;(void)r;(void)w;(void)e;(void)t;assert(0);return -1;
}
static int send(int fd,const void *p,size_t n,int flags) { (void)fd;(void)p;(void)n;(void)flags;assert(0);return -1; }
static int recv(int fd,void *p,size_t n,int flags) { (void)fd;(void)p;(void)n;(void)flags;assert(0);return -1; }
DIAGNOSTICS
WORKERS
static void reset(void) {
    thread=deletes=deinit=listener_closes=accept_calls=diag_reads=handled=client_closes=sleeps=0;
    queued_count=popped=0; accept_entry=worker_entry=NULL;
}
int main(void) {
    for(fail_stage=1;fail_stage<=5;fail_stage++) {
        reset(); assert(a1_http_start()==0 && accept_entry && !worker_entry);
        thread=1; accept_entry(0);
        assert(deletes==1 && !worker_entry && listener_closes==(fail_stage!=1));
        assert(deinit==(fail_stage==5));
    }
    reset();fail_stage=0;assert(a1_http_start()==0);
    thread=1;if(!setjmp(stop))accept_entry(0);
    assert(worker_entry && queued_count==2 && sleeps==1 && diag_reads==4);
    thread=2;if(!setjmp(stop))worker_entry(0);
    assert(handled==2 && client_closes==2 && !listener_closes && !deletes);
    assert(http_diag_snapshot().limit==24);
    puts("A1 listener ownership, queued fd handoff, startup cleanup: OK");
}
'''

SEND = PRELUDE + r'''
static uint32_t now, start;
static unsigned selects, sends, send_step, send_limit, timeout_call, select_interrupt;
static unsigned timeouts[256], fail_fcntl, nonblocking, transient, fatal_send, late_select, send_capacity;
static char output[65536];static size_t used;
static uint32_t mico_rtos_get_time(void) { return now; }
static int fcntl(int fd,int command,int flags) {
    assert(fd==7 && command==F_SETFL && flags==O_NONBLOCK);
    if(fail_fcntl)return -1;nonblocking=1;return 0;
}
static int select(int n,fd_set *r,fd_set *w,fd_set *e,struct timeval *t) {
    assert(n==8 && !r && w && !e && FD_ISSET(7,w) && selects<256);
    unsigned ms=t->tv_sec*1000+t->tv_usec/1000;
    assert(ms>0 && ms<=HTTP_RESPONSE_TIMEOUT_MS);
    timeouts[selects++]=ms;
    if(selects==timeout_call) { now+=ms;return 0; }
    if(selects==select_interrupt) { now+=100;errno=EINTR;return -1; }
    if(late_select)now+=ms;
    return 1;
}
static int send(int fd,const void *p,size_t n,int flags) {
    assert(fd==7 && nonblocking && flags==0 && now-start<HTTP_RESPONSE_TIMEOUT_MS);
    ++sends;
    if(fatal_send) { errno=EPIPE;return -1; }
    if(transient) { errno=transient--==2?EAGAIN:EINTR;return -1; }
    /* Model a writable embedded socket that cannot enqueue an oversized write. */
    if(send_capacity && n>send_capacity) { now+=1000;errno=EAGAIN;return -1; }
    if(send_limit && n>send_limit)n=send_limit;
    assert(used+n<sizeof(output));memcpy(output+used,p,n);used+=n;now+=send_step;return n;
}
FUNCTIONS
static void reset(uint32_t initial) {
    start=now=initial;selects=sends=send_step=send_limit=timeout_call=select_interrupt=0;
    fail_fcntl=nonblocking=transient=fatal_send=late_select=send_capacity=0;used=0;memset(output,0,sizeof(output));
}
int main(void) {
    reset(100);send_limit=1;transient=2;select_interrupt=1;
    assert(response_send(7,"head",4,"body",4) && !strcmp(output,"headbody"));
    assert(timeouts[0]==10000 && timeouts[1]==9900);
    reset(100);send_step=7000;timeout_call=2;
    assert(!response_send(7,"head",4,"body",4) && sends==1 && now-start==10000);
    assert(timeouts[0]==10000 && timeouts[1]==3000);
    reset(100);send_limit=1;send_step=1000;
    assert(!response_send(7,"header",6,"longbody",8) && sends==10 && now-start==10000);
    reset(UINT32_MAX-50u);send_step=20;timeout_call=2;
    assert(!response_send(7,"head",4,"body",4) && now-start==10000);
    assert(timeouts[1]==9980);
    reset(100);late_select=1;
    assert(!response_send(7,"head",4,"body",4) && sends==0 && now-start==10000);
    reset(100);fail_fcntl=1;
    assert(!response_send(7,"head",4,"body",4) && !selects && !sends);
    reset(100);fatal_send=1;
    assert(!response_send(7,"head",4,"body",4) && sends==1);
    reset(100);assert(response_send(7,"head",4,NULL,0) && sends==1);
    reset(100);assert(response(7,200,"text/plain","payload",7,false));
    assert(strstr(output,"HTTP/1.1 200 OK\r\n") && strstr(output,"\r\n\r\npayload"));
    EXTRA_CASES
    puts("A1 nonblocking response: partial/EAGAIN/EINTR/error/shared deadline/tick wrap: OK");
}
'''


def fragment(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


class A1HttpOwnerTest(unittest.TestCase):
    def run_harness(self, harness, source):
        defines = '\n'.join(line for line in source.splitlines() if line.startswith('#define HTTP_'))
        with tempfile.TemporaryDirectory() as temporary:
            probe = Path(temporary) / 'probe.c'
            probe.write_text(harness.replace('/*DEFINES*/', defines))
            binary = Path(temporary) / 'probe'
            command = [os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra', '-Werror',
                       '-Wno-misleading-indentation', str(probe), '-o', str(binary)]
            if os.environ.get('SANITIZE') == '1':
                command += ['-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined']
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True)

    def test_listener_owned_by_accept_thread(self):
        for product in ('A1', 'M1'):
            with self.subTest(product=product):
                source = (ROOT / product / 'http.c').read_text()
                harness = OWNER.replace('a1_', product.lower() + '_').replace('A1 ', product + ' ')
                harness = harness.replace('DIAGNOSTICS', fragment(source,
                    'static volatile uint32_t http_accept_ok;', f'int {product.lower()}_socket_read('))
                harness = harness.replace('WORKERS', source[source.index('/* Keep accept responsive'):])
                self.run_harness(harness.replace(PRELUDE, PRELUDE + '\n/*DEFINES*/\n'), source)

    def test_entire_response_has_one_nonblocking_deadline(self):
        for product in ('A1', 'M1'):
            with self.subTest(product=product):
                source = (ROOT / product / 'http.c').read_text()
                functions = fragment(source, 'static bool response_send_until(', 'static void message(')
                harness = SEND.replace('A1 ', product + ' ').replace(PRELUDE, PRELUDE + '\n/*DEFINES*/\n')
                self.run_harness(harness.replace('FUNCTIONS', functions).replace('EXTRA_CASES', ''), source)

    def test_a1_large_page_with_limited_send_capacity(self):
        source = (ROOT / 'A1/http.c').read_text()
        functions = fragment(source, 'static bool response_send_until(', 'static void message(')
        cases = r'''
    static unsigned char page[58267];
    for(size_t i=0;i<sizeof(page);i++)page[i]=(unsigned char)(i*37u+i/251u);
    for(unsigned short_write=0;short_write<2;short_write++) {
        reset(100);send_capacity=1024;transient=2;send_step=1;
        send_limit=short_write?509:0;
        assert(response(7,200,"text/html; charset=utf-8",page,sizeof(page),true));
        const char *body=strstr(output,"\r\n\r\n");assert(body);body+=4;
        assert(strstr(output,"Content-Length: 58267\r\n"));
        assert(strstr(output,"Content-Encoding: gzip\r\n"));
        assert(used-(size_t)(body-output)==sizeof(page));
        assert(!memcmp(body,page,sizeof(page)) && now-start<10000);
    }
'''
        harness = SEND.replace(PRELUDE, PRELUDE + '\n/*DEFINES*/\n')
        self.run_harness(harness.replace('FUNCTIONS', functions).replace('EXTRA_CASES', cases), source)


if __name__ == '__main__':
    unittest.main()

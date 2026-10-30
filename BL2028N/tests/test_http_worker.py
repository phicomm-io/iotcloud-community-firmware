#!/usr/bin/env python3
"""Run the native HTTP worker, parser and OTA callback over real TCP sockets."""
import argparse
import json
from pathlib import Path
import re
import subprocess
from test_ota_callback import STUBS as FLASH_STUBS

HOST = r'''
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <time.h>
#define ENABLE_SEND_POSTANDGET 1
#define ADDLOG_ERROR(...) ((void)0)
#define ADDLOG_INFO(...) ((void)0)
#define ADDLOG_DEBUG(...) ((void)0)
#define ADDLOG_WARN(...) ((void)0)
typedef uintptr_t beken_thread_arg_t;
static unsigned thread_ends;
static void rtos_delete_thread(void *p) {(void)p;++thread_ends;}
static uint32_t rtos_get_time(void) {struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint32_t)((uint64_t)t.tv_sec*1000+t.tv_nsec/1000000);}
static void nap(unsigned ms) {struct timespec t={ms/1000,(ms%1000)*1000000};while(nanosleep(&t,&t)<0)assert(errno==EINTR);}
'''

NET = r'''
static int net_connect(utils_network_pt n) {
 int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);
 struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=htons(n->port);
 if(connect(fd,(struct sockaddr*)&a,sizeof a)){close(fd);return -1;}
 n->handle=fd;return 0;
}
static int net_read(utils_network_pt n,char *p,uint32_t z,uint32_t t){return HAL_TCP_Read(n->handle,p,z,t);}
static int net_write(utils_network_pt n,const char *p,uint32_t z,uint32_t t){(void)t;return (int)send(n->handle,p,z,MSG_NOSIGNAL);}
static int net_close(utils_network_pt n){if(n->handle)close(n->handle);n->handle=0;return 0;}
int iotx_net_init(utils_network_pt n,const char *host,uint16_t port,const char *ca){*n=(utils_network_t){0};n->pHostAddress=host;n->port=port;n->ca_crt=ca;n->doConnect=net_connect;n->doRead=net_read;n->doWrite=net_write;n->doDisconnect=net_close;return 0;}
'''

CASES = r'''
static const unsigned char *rbl;
static size_t rbl_size;
static int listen_fd,mode,errors,ends;
static void send_bytes(int fd,const void *p,size_t n){const char *b=p;while(n){ssize_t k=send(fd,b,n,MSG_NOSIGNAL);if(k<=0)return;b+=k;n-=(size_t)k;}}
static void *server(void *unused) {
 (void)unused;int fd=accept(listen_fd,NULL,NULL);assert(fd>=0);char req[1024];assert(recv(fd,req,sizeof req,0)>0);
 char head[128];int n=snprintf(head,sizeof head,"HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n",rbl_size);
 if(mode==0)nap(230); /* no headers */
 else if(mode==1){send_bytes(fd,"HTTP/1.1 200 OK\r\nContent-Len",27);nap(230);}
 else {send_bytes(fd,head,(size_t)n);
  if(mode==2){send_bytes(fd,rbl,1024);nap(230);}
  else if(mode==3)send_bytes(fd,rbl,1024); /* truncated EOF */
  else if(mode==4){for(unsigned i=0;i<30;++i){send_bytes(fd,rbl+i,1);nap(15);}}
  else send_bytes(fd,rbl,rbl_size);
 }
 close(fd);return NULL;
}
static int callback(httprequest_t *request){if(request->state<0)++errors;if(request->state==2)++ends;return myhttpclientcallback(request);}
static void run(int scenario) {
 mode=scenario;errors=ends=0;memset(flash,0xa5,sizeof flash);memset(erased,0,sizeof erased);
 assert(!sector && !http_buf);reboots=erases=progress=total_bytes=0;
 listen_fd=socket(AF_INET,SOCK_STREAM,0);assert(listen_fd>=0);
 struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 assert(!bind(listen_fd,(struct sockaddr*)&a,sizeof a));assert(!listen(listen_fd,1));socklen_t alen=sizeof a;assert(!getsockname(listen_fd,(struct sockaddr*)&a,&alen));
 pthread_t thread;assert(!pthread_create(&thread,NULL,server,NULL));
 http_buf=malloc(513);assert(http_buf);memset(&httprequest,0,sizeof httprequest);
 char url[96];snprintf(url,sizeof url,"http://127.0.0.1:%u/firmware.rbl",ntohs(a.sin_port));
 httprequest.url=url;httprequest.port=ntohs(a.sin_port);httprequest.method=HTTPCLIENT_GET;
 httprequest.timeout=scenario==5?3000:160;httprequest.client_data.response_buf=http_buf;httprequest.client_data.response_buf_len=512;httprequest.data_callback=callback;
 uint32_t start=rtos_get_time();httprequest_thread((beken_thread_arg_t)&httprequest);uint32_t elapsed=rtos_get_time()-start;
 assert(!pthread_join(thread,NULL));close(listen_fd);
 assert(ends==1 && !sector && !http_buf && !httprequest.client.net.handle);
 assert(!httprequest.client_data.response_buf && protection==FLASH_UNPROTECT_LAST_BLOCK);
 assert(reboots==(unsigned)(scenario==5));
 if(scenario<5){assert(errors==1 && progress==0 && elapsed<1000);if(scenario!=3)assert(elapsed>=130);}
 else {assert(!errors && !memcmp(flash+START_ADR_OF_BK_PARTITION_OTA,rbl,rbl_size));}
 for(unsigned i=0;i<START_ADR_OF_BK_PARTITION_OTA;++i)assert(flash[i]==0xa5);
 for(unsigned i=LFS_START;i<sizeof flash;++i)assert(flash[i]==0xa5);
 printf("scenario %d PASS elapsed=%u ms errors=%d reboots=%u\n",scenario,elapsed,errors,reboots);
}
int main(int argc,char **argv){assert(argc==2);signal(SIGPIPE,SIG_IGN);FILE *f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));rbl_size=(size_t)ftell(f);assert(rbl_size>8192 && rbl_size<600000);rewind(f);unsigned char *data=malloc(rbl_size);assert(data);assert(fread(data,1,rbl_size,f)==rbl_size);fclose(f);rbl=data;for(int i=0;i<6;++i)run(i);assert(thread_ends==6);free(data);puts("PASS: native HTTP worker and OTA callback, 5 real-socket failures then complete retry");}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--app-source', type=Path, required=True)
    p.add_argument('--firmware', type=Path, required=True)
    p.add_argument('--run-dir', type=Path, required=True)
    a = p.parse_args(); run = a.run_dir.resolve(); run.mkdir(parents=True, exist_ok=True)
    app = a.app_source
    def header(name):
        return re.sub(r'^\s*#include[^\n]*', '', (app/'src/httpclient'/name).read_text(), flags=re.M)
    flash = FLASH_STUBS[:FLASH_STUBS.index('typedef struct {int response_code;')]
    prefix = flash.index('#include <assert.h>')
    flash = flash[prefix:]
    worker = (app/'src/httpclient/http_client.c').read_text()
    worker = worker[worker.index('#define HTTPCLIENT_MIN'):worker.index('int HTTPClient_Async_SendGeneric(')]
    timer = (app/'src/httpclient/utils_timer.c').read_text()
    timer = timer[timer.index('void iotx_time_start('):timer.rindex('#endif')]
    net = (app/'src/httpclient/utils_net.c').read_text()
    net = net[net.index('int32_t HAL_TCP_Read('):net.index('\n/*** TCP connection ***/')]
    ota = (app/'src/hal/bk7231/hal_ota_bk7231.c').read_text()
    ota = ota[ota.index('static unsigned char *sector'):ota.index('  // NOTE: these MUST persist')]
    source = run/'native-http-worker.c'
    source.write_text('#define _POSIX_C_SOURCE 200809L\n'+flash+HOST+header('iot_export_errno.h')+header('utils_net.h')+header('http_client.h')+header('utils_timer.h')+timer+net+NET+worker+ota+CASES)
    exe = run/'native-http-worker'
    with (run/'compiler.log').open('w') as log:
        subprocess.run(['clang','-std=c99','-g','-O1','-Wall','-Wextra','-Werror','-Wno-sign-compare','-Wno-unused-parameter','-Wno-unused-variable','-Wno-unused-function','-Wno-unused-but-set-variable','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread','-I',str(Path(__file__).resolve().parents[1]/'src'),str(source),'-o',str(exe)],stdout=log,stderr=subprocess.STDOUT,check=True)
    result = subprocess.run([str(exe),str(a.firmware.resolve())],text=True,capture_output=True,timeout=15)
    (run/'test.log').write_text(result.stdout+result.stderr)
    (run/'result.json').write_text(json.dumps({'status':'PASS' if not result.returncode else 'FAIL','native_worker_parser_timer_reader_ota_verbatim':True,'real_tcp_sockets':True,'asan_ubsan':True,'hardware_verified':False},indent=2)+'\n')
    result.check_returncode(); print(result.stdout,end='')

if __name__ == '__main__': main()

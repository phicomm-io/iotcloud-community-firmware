#!/usr/bin/env python3
"""Run native OTA allocation/startup failure and immediate-completion paths."""
import argparse
import json
from pathlib import Path
import subprocess

STUBS = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define LOG_INFO 0
#define LOG_FEATURE_OTA 0
#define HTTPCLIENT_GET 0
#define addLogAdv(...) ((void)0)
typedef struct {int unused;} httpclient_t;
typedef struct {char *response_buf;unsigned response_buf_len;
 const char *post_buf,*post_content_type;unsigned post_buf_len;} httpclient_data_t;
typedef struct request {int state,port,method,timeout;const char *url;
 httpclient_t client;httpclient_data_t client_data;
 int (*data_callback)(struct request *);} httprequest_t;
static httprequest_t httprequest;
static char *http_buf;
static int allocation_failed,thread_failed,complete_immediately,progress=-1;
static unsigned allocations,releases,starts;
static void *os_malloc(size_t n){assert(n==513);if(allocation_failed)return NULL;++allocations;return malloc(n);}
static void os_free(void *p){if(p){++releases;free(p);}}
static void OTA_SetTotalBytes(int n){(void)n;}
static void OTA_ResetProgress(void){progress=-1;}
static void OTA_IncrementProgress(int n){progress+=n;}
static void HTTPClient_SetCustomHeader(httpclient_t *c,const char *h){(void)c;(void)h;}
static int myhttpclientcallback(httprequest_t *r){(void)r;return 0;}
static void ph_ota_release_response(httprequest_t *);
static int HTTPClient_Async_SendGeneric(httprequest_t *r){
 ++starts;assert(progress==0 && r->timeout==120000 && r->client_data.response_buf_len==512);
 if(thread_failed)return -1;
 if(complete_immediately){ph_ota_release_response(r);OTA_ResetProgress();}
 return 0;
}
'''
CASES = r'''
int main(void){
 httprequest.state=1;otarequest("http://example/firmware.rbl");assert(!allocations && !starts);
 httprequest.state=2;allocation_failed=1;otarequest("http://example/firmware.rbl");
 assert(!allocations && !starts && progress==-1 && !http_buf);
 allocation_failed=0;thread_failed=1;otarequest("http://example/firmware.rbl");
 assert(allocations==1 && releases==1 && starts==1 && progress==-1 && !http_buf && !httprequest.client_data.response_buf);
 thread_failed=0;otarequest("http://example/firmware.rbl");
 assert(allocations==2 && starts==2 && progress==0 && http_buf);
 ph_ota_release_response(&httprequest);OTA_ResetProgress();
 complete_immediately=1;otarequest("http://example/firmware.rbl");
 assert(allocations==3 && releases==3 && starts==3 && progress==-1 && !http_buf);
 puts("PASS: native OTA busy/allocation/thread failure, retry, immediate completion and buffer ownership");
}
'''

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--app-source',type=Path,required=True)
    p.add_argument('--run-dir',type=Path,required=True)
    args=p.parse_args();run=args.run_dir.resolve();run.mkdir(parents=True,exist_ok=True)
    text=(args.app_source/'src/hal/bk7231/hal_ota_bk7231.c').read_text()
    helper=text[text.index('static void ph_ota_release_response('):text.index('\nint myhttpclientcallback(')]
    request=text[text.index('static char url[256]'):text.index('\nint http_rest_post_flash(')]
    source=run/'ota-request.c';source.write_text(STUBS+helper+request+CASES)
    executable=run/'ota-request'
    with (run/'compiler.log').open('w') as log:
        subprocess.run(['clang','-std=c99','-O1','-g','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined',str(source),'-o',str(executable)],
                       stdout=log,stderr=subprocess.STDOUT,check=True)
    result=subprocess.run([str(executable)],capture_output=True,text=True,timeout=5)
    (run/'test.log').write_text(result.stdout+result.stderr)
    (run/'result.json').write_text(json.dumps({'status':'PASS' if result.returncode==0 else 'FAIL',
                                              'native_request_verbatim':True,'asan_ubsan':True},indent=2)+'\n')
    result.check_returncode();print(result.stdout,end='')

if __name__=='__main__':main()

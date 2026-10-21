"""Preserve the TC1 archive's Basic authentication when moving to upstream MiCO."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_tc1_sdk_migration import ROOT, function


class SDKAuth(unittest.TestCase):
    def test_authentication_before_every_handler_and_bounded_header(self):
        sdk = Path(os.environ.get('TC1_SDK_DIR', ROOT / 'build/tc1-sdk-3.6.4/mico-os'))
        path = sdk / 'libraries/daemons/http_server'
        if not path.exists():
            self.skipTest('build TC1 first')
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#define kNoErr 0
#define kInProgressErr 1
#define WM_E_HTTPD_NO_HANDLER 2
#define HTTPD_DONE 3
#define HTTPD_MAX_MESSAGE 256
#define HTTPD_MAX_AUTH_LENGTH 64
#define MAX_WSGI_HANDLERS 1
#define APP_HTTP_FLAGS_NO_EXACT_MATCH 1
#define HTTPD_REQ_TYPE_OPTIONS 32
#define HTTP_RES_200 "HTTP/1.1 200 OK\r\n"
#define HTTPD_REQ_TYPE_HEAD 1
#define HTTPD_REQ_TYPE_GET 2
#define HTTPD_REQ_TYPE_POST 3
#define HTTPD_REQ_TYPE_PUT 4
#define HTTPD_REQ_TYPE_DELETE 5
#define ISO_nl '\n'
#define ISO_cr '\r'
#define HTTP_CHUNKED "chunked"
#define TRUE 1
#define httpd_d(...) ((void)0)
typedef struct httpd_request httpd_request_t;
struct httpd_wsgi_call { const char *uri; int http_flags; int (*get_handler)(httpd_request_t*),(*set_handler)(httpd_request_t*),(*put_handler)(httpd_request_t*),(*delete_handler)(httpd_request_t*); };
struct httpd_request { char filename[32]; int sock,type,hdr_parsed; char authorization[65];char origin[160]; int remaining_bytes,body_nbytes,agent; char content_type[128]; bool if_none_match;unsigned etag_val;int chunked;const struct httpd_wsgi_call *wsgi; };
static struct httpd_wsgi_call *calls[1];
static const char http_content_len[]="Content-Length:",http_user_agent[]="User-Agent:",http_content_type[]="Content-Type:",http_encoding[]="Transfer-Encoding:";

static int handlers,denied,parse_error;
static char header[256];
static const char *credential="YWRtaW46c2VjcmV0";
static const char *get_httpd_auth(void) {return credential;}
static char *httpd_skip_absolute_http_path(char *p) {return p;}
static void httpd_parse_useragent(char *p,int *agent) {(void)p;(void)agent;}
static const char *phicomm_http_cors(const char *origin) {return !strcmp(origin,"https://phicomm-io.github.io")?"Access-Control-Allow-Origin: https://phicomm-io.github.io\r\n":"";}
static int httpd_send_all_header(httpd_request_t *r,const char *p,int n,const char *mime) {(void)r;(void)n;(void)mime;if(strstr(p,"401"))denied++;return 0;}
'''
        harness += function((path / 'http_parse.c').read_text(), 'static int __httpd_parse_hdr_tags(')
        harness += r'''
static int httpd_parse_hdr_tags(httpd_request_t *r,int fd,char *scratch,int size) {
 (void)fd;(void)scratch;(void)size;if(parse_error)return -1;uint8_t done=0;
 return __httpd_parse_hdr_tags(header,strlen(header),r,&done);
}
'''
        source = (path / 'httpd_wsgi.c').read_text()
        harness += function(source, 'static int get_matching_chars(')
        harness += function(source, 'int httpd_wsgi(')
        harness += r'''
static int handler(httpd_request_t *r) {assert(!credential||r->hdr_parsed);handlers++;return 0;}
int main(void) {
 struct httpd_wsgi_call route={"/control",0,handler,handler,handler,handler};calls[0]=&route;
 for(int type=1;type<=5;type++) {
  for(int mode=0;mode<4;mode++) {
   httpd_request_t r={.filename="/control",.type=type};header[0]=0;parse_error=0;
   if(mode==1)strcpy(header,"Authorization: Basic wrong\r\n");
   if(mode==2)parse_error=1;
   if(mode==3)strcpy(header,"authorization: basic YWRtaW46c2VjcmV0\r\n");
   handlers=denied=0;int result=httpd_wsgi(&r);
   if(mode==3)assert(result==HTTPD_DONE && handlers==1 && !denied);
   else assert(!handlers && (mode==2?result<0:denied==1));
  }
 }
 httpd_request_t r={.filename="/control",.type=HTTPD_REQ_TYPE_POST,.remaining_bytes=123};
 memset(header,'a',sizeof header);memcpy(header,"Authorization: Basic ",21);header[sizeof header-1]=0;
 assert(httpd_wsgi(&r)==HTTPD_DONE && strlen(r.authorization)==64 && r.remaining_bytes==123);
 credential=NULL;handlers=0;r.hdr_parsed=0;assert(httpd_wsgi(&r)==HTTPD_DONE && handlers==1);
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / 'auth.c').write_text(harness)
            subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                            str(directory / 'auth.c'), '-o', str(directory / 'auth')], check=True)
            subprocess.run([str(directory / 'auth')], check=True, timeout=10)

    def test_response_headers_without_origin_never_send_empty_buffer(self):
        sdk = Path(os.environ.get('TC1_SDK_DIR', ROOT / 'build/tc1-sdk-3.6.4/mico-os'))
        path = sdk / 'libraries/daemons/http_server/httpd_wsgi.c'
        if not path.exists():
            self.skipTest('build TC1 first')
        harness = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define kNoErr 0
#define HTTPD_REQ_TYPE_GET 1
#define HTTPD_REQ_TYPE_HEAD 2
#define httpd_d(...) ((void)0)
struct route {int hdr_fields;const char *uri;};
typedef struct {int sock,type,hdr_parsed;char origin[160];struct route *wsgi;} httpd_request_t;
static char output[2048];
static const char *http_header_keep_alive_ctrl="Connection: close\r\n";
static const char *phicomm_http_cors(const char *origin){return !strcmp(origin,"https://phicomm-io.github.io")?"Access-Control-Allow-Origin: https://phicomm-io.github.io\r\n":"";}
/* SDK httpd_send treats send(..., 0) == 0 as a transport failure. */
static int httpd_send(int sock,const char *buf,int n){(void)sock;assert(n>0);strncat(output,buf,n);return 0;}
static int httpd_purge_headers(int sock){(void)sock;return 0;}
static int httpd_send_default_headers(int sock,int flags){(void)sock;(void)flags;return 0;}
static int httpd_send_header(int sock,const char *key,const char *value){(void)sock;strcat(output,key);strcat(output,": ");strcat(output,value);strcat(output,"\r\n");return 0;}
static int httpd_send_crlf(int sock){(void)sock;strcat(output,"\r\n");return 0;}
static int chunked_encoding(httpd_request_t *r){(void)r;return 0;}
static int httpd_send_chunk(int sock,const char *body,int n){(void)sock;(void)body;(void)n;assert(0);return -1;}
'''
        source=path.read_text()
        harness += function(source, 'int httpd_send_all_header(')
        harness += function(source, 'int httpd_send_response(')
        harness += r'''
int main(void){
 struct route route={0,"/api/v1/info"};
 const char *origins[]={"","https://untrusted.example","https://phicomm-io.github.io"};
 for(int i=0;i<3;i++){
  httpd_request_t r={.type=HTTPD_REQ_TYPE_GET,.hdr_parsed=1,.wsgi=&route};strcpy(r.origin,origins[i]);
  for(int mode=0;mode<2;mode++){
   output[0]=0;
   int rc=mode?httpd_send_response(&r,"HTTP/1.1 200 OK\r\n","{}",2,"application/json"):httpd_send_all_header(&r,"HTTP/1.1 200 OK\r\n",2,"application/json");
   assert(rc==0 && strstr(output,"Content-Length: 2\r\n") && strstr(output,"Content-Type: application/json\r\n"));
   assert((strstr(output,"Access-Control-Allow-Origin:")!=NULL)==(i==2));
   if(mode)assert(strstr(output,"\r\n\r\n{}"));
  }
 }
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            directory=Path(temporary)
            (directory/'cors.c').write_text(harness)
            subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror',str(directory/'cors.c'),'-o',str(directory/'cors')],check=True)
            subprocess.run([str(directory/'cors')],check=True,timeout=10)


if __name__ == '__main__':
    unittest.main()

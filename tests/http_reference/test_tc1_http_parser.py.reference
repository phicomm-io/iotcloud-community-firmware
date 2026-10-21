"""Run actual SDK parsers with malformed bytes and bounded header input."""
import unittest
from tests import test_memory_admission as admission
from tests.test_tc1_sdk_migration import function

PRELUDE=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define TRUE true
#define ISO_nl '\n'
#define ISO_cr '\r'
#define ISO_space ' '
#define ISO_tab '\t'
#define kNoErr 0
#define kInProgressErr 12
#define httpd_d(...) ((void)0)
#define HTTPD_MAX_VAL_LENGTH 31
#define HTTPD_MAX_AUTH_LENGTH 64
#define HTTP_CHUNKED "chunked"
typedef struct {char product[32],version[32];} httpd_useragent_t;
typedef struct {int body_nbytes,remaining_bytes;httpd_useragent_t agent;unsigned chunked;char content_type[128];unsigned if_none_match,etag_val;char origin[160],authorization[65];} httpd_request_t;
static const char http_content_len[]="Content-Length:",http_user_agent[]="User-Agent:",http_content_type[]="Content-Type:",http_encoding[]="Transfer-Encoding:";
'''

class Parser(unittest.TestCase):
 def source(self,name):return admission.MemoryAdmission().sdk_http_source(name)

 def run_c(self,s):admission.MemoryAdmission().run_c(s)
 def test_unknown_resource_closes_without_reading_body(self):
  code=function(self.source('httpd_handle.c'),'int httpd_handle_message(')
  self.run_c(r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#define kNoErr 0
#define kInProgressErr 12
#define HTTPD_DONE 1
#define WM_E_HTTPD_NOTSUPP 13
#define WM_E_HTTPD_NO_HANDLER 14
#define HTTP_505 505
#define HTTP_500 500
#define HTTP_404 404
#define HTTPD_HDR_ADD_CONN_CLOSE 1
#define httpd_d(...) ((void)0)
#define httpd_set_error(...) ((void)0)
struct wsgi {unsigned hdr_fields;};
typedef struct {int sock;char filename[32];struct wsgi *wsgi;} httpd_request_t;
static httpd_request_t httpd_req;static unsigned replies;
static int htsys_getln_soc(int fd,char *p,int n){assert(fd==3&&p&&n==128);return 24;}
static int httpd_parse_hdr_main(char *p,httpd_request_t *r){(void)p;strcpy(r->filename,"/missing");return 0;}
static int httpd_wsgi(httpd_request_t *r){assert(r==&httpd_req);return -WM_E_HTTPD_NO_HANDLER;}
static int httpd_send_error(int fd,int code){assert(fd==3&&code==HTTP_404);replies++;return 0;}
'''+code+r'''
int main(void){assert(httpd_handle_message(3)==HTTPD_DONE&&replies==1);
 puts("TC1 unknown resource: closes after 404 without header/body drain: OK");}
''')

 def test_useragent_empty_compatible_and_random_inputs(self):
  source=self.source('http_parse.c')
  code=function(source,'static char *__httpd_parse_useragent(')+'\n#define MOZ_COMPATIBLE "compatible;"\n'+function(source,'void httpd_parse_useragent(')+'\n'+function(source,'static int __httpd_parse_hdr_tags(')
  self.run_c(PRELUDE+code+r'''
int main(void){
 const char *cases[]={""," ","Mozilla/5.0 (compatible;)","Mozilla/5.0 (compatible; )","Mozilla/5.0 (compatible; MSIE)","Mozilla/5.0 (compatible; MSIE )","Mozilla/5.0 MSIE ","MSIE ","Opera ","Mozilla/", "()", "Mozilla/5.0 (compatible; MSIE;)", "Mozilla/5.0 (compatible; MSIE 8.0; Windows NT 6.0)"};
 for(unsigned n=0;n<sizeof(cases)/sizeof(*cases);n++){
  size_t length=strlen(cases[n]);char *p=malloc(length+1);strcpy(p,cases[n]);httpd_useragent_t agent;
  memset(&agent,0xff,sizeof(agent));httpd_parse_useragent(p,&agent);assert(agent.product[31]==0&&agent.version[31]==0);free(p);
 }
 httpd_request_t req={0};uint8_t done=0;char empty[]="User-Agent:";
 assert(__httpd_parse_hdr_tags(empty,strlen(empty),&req,&done)==0);
 uint32_t seed=1;const char chars[]="abcMozillaMSIE Opera/();, +[]0123";
 for(unsigned n=0;n<20000;n++){
  unsigned length=n%128;char *p=malloc(length+1);
  for(unsigned j=0;j<length;j++){seed=seed*1664525u+1013904223u;p[j]=chars[seed%(sizeof(chars)-1)];}p[length]=0;
  httpd_useragent_t agent;httpd_parse_useragent(p,&agent);free(p);
 }
 puts("TC1 actual User-Agent: exact-sized empty/compatible cases and 20000 malformed inputs: OK");
}
''')

 def test_content_length_rejects_signed_overflow_and_junk(self):
  source=self.source('http_parse.c')
  self.run_c(PRELUDE+'static void httpd_parse_useragent(char *p,httpd_useragent_t *a){(void)p;(void)a;}\n'+function(source,'static int __httpd_parse_hdr_tags(')+r'''
int main(void){
 const char *invalid[]={"-1","+1","2147483648","9999999999999999999999999999999999999999999","1x","","  "};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++){char line[128];snprintf(line,sizeof(line),"Content-Length: %s",invalid[i]);httpd_request_t req={0};uint8_t done=0;assert(__httpd_parse_hdr_tags(line,strlen(line),&req,&done)<0);}
 const char *valid[]={"0","8192","2147483647"};
 for(unsigned i=0;i<3;i++){char line[128];snprintf(line,sizeof(line),"Content-Length:\t%s ",valid[i]);httpd_request_t req={0};uint8_t done=0;assert(!__httpd_parse_hdr_tags(line,strlen(line),&req,&done));assert(req.body_nbytes==atoi(valid[i]));}
 puts("TC1 Content-Length rejects signed, overflow and trailing junk before body allocation: OK");
}
''')

 def test_request_version_token_has_space_for_terminator(self):
  source=self.source('http_parse.c')
  code='\n'.join(function(source,n) for n in ('static const char *httpd_next_token(', 'static int httpd_token_cpy(', 'int httpd_parse_hdr_main('))
  self.run_c(r'''
#include <assert.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#define TRUE true
#define ISO_nl '\n'
#define ISO_cr '\r'
#define ISO_space ' '
#define ISO_tab '\t'
#define iswhite(c) ((c)==' '||(c)=='\t'||(c)=='\n'||(c)=='\r')
#define kNoErr 0
#define kInProgressErr 12
#define WM_E_HTTPD_HDR_FNAME 13
#define WM_E_HTTPD_NOTSUPP 14
#define HTTPD_MAX_URI_LENGTH 128
#define httpd_d(...) ((void)0)
#define httpd_set_error(...) ((void)0)
typedef struct {int type,sock;char filename[129];} httpd_request_t;
static const char http_10[]="HTTP/1.0";
static int httpd_verify_req_type(const char *p,int *t){(void)p;*t=1;return 0;}
static int httpd_purge_headers(int fd){(void)fd;return 0;}
'''+code+r'''
int main(void){httpd_request_t req={0};
 assert(!httpd_parse_hdr_main("GET /api/v1/status 1234567890 ",&req));
 assert(httpd_parse_hdr_main("GET /api/v1/status 12345678901 ",&req)<0);
 assert(httpd_parse_hdr_main("GET /api/v1/status HTTP/1.0",&req)<0);
 puts("TC1 exact-limit request version terminator and oversized token: OK");}
''')

 def test_line_truncation_and_slow_drip_are_bounded(self):
  code=function(self.source('httpd_sys.c'),'int htsys_getln_soc(')
  self.run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define ISO_nl '\n'
#define ISO_cr '\r'
#define kInProgressErr 12
#define httpd_d(...) ((void)0)
static const char *input;static unsigned at,step;static uint32_t now;
static uint32_t mico_rtos_get_time(void){return now;}
static int httpd_recv(int fd,char *p,unsigned n,int flags){(void)fd;(void)flags;assert(n==1);now+=step;if(!input[at])return 0;*p=input[at++];return 1;}
'''+code+r'''
int main(void){char output[8];
 input="abc\r\n";step=0;assert(htsys_getln_soc(1,output,sizeof(output))==3&&!strcmp(output,"abc"));
 at=0;input="abcdefghijk\r\n";assert(htsys_getln_soc(1,output,sizeof(output))<0&&at==7&&output[7]==0);
 at=0;step=2000;input="abcdef\r\n";now=UINT32_MAX-999;assert(htsys_getln_soc(1,output,sizeof(output))<0&&at==3);
 assert(htsys_getln_soc(1,NULL,8)<0&&htsys_getln_soc(1,output,1)<0);
 puts("TC1 actual line reader rejects truncation and 5-second drip across tick wrap: OK");}
''')

 def test_aggregate_headers_have_time_byte_and_line_limits(self):
  code=function(self.source('http_parse.c'),'int httpd_parse_hdr_tags(')
  self.run_c(PRELUDE+r'''
static unsigned calls,mode;static uint32_t now;
static uint32_t mico_rtos_get_time(void){return now;}
static int htsys_getln_soc(int fd,char *p,int n){(void)fd;(void)n;calls++;if(mode==1)now+=1000;strcpy(p,"X: x");return mode==2?100:4;}
static int __httpd_parse_hdr_tags(char *p,int n,httpd_request_t *r,uint8_t *done){(void)p;(void)n;(void)r;if(mode==0&&calls==3)*done=1;return 0;}
'''+code+r'''
int main(void){httpd_request_t req={0};char line[128];
 assert(!httpd_parse_hdr_tags(&req,1,line,sizeof(line))&&calls==3);
 calls=0;mode=1;now=UINT32_MAX-999;assert(httpd_parse_hdr_tags(&req,1,line,sizeof(line))<0&&calls==5);
 calls=0;mode=2;now=0;assert(httpd_parse_hdr_tags(&req,1,line,sizeof(line))<0&&calls==41);
 calls=0;mode=3;assert(httpd_parse_hdr_tags(&req,1,line,sizeof(line))<0&&calls==64);
 (void)http_content_len;(void)http_user_agent;(void)http_content_type;(void)http_encoding;
 puts("TC1 aggregate headers: 5-second, 4096-byte and 64-line bounds: OK");}
''')

 def test_purged_headers_stop_at_byte_and_time_limits(self):
  code=function(self.source('httpd_wsgi.c'),'int httpd_purge_headers(')
  self.run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define kNoErr 0
#define kInProgressErr 12
typedef enum {ANY_OTHER_CHAR,FIRST_CR_FOUND,FIRST_LF_FOUND,SECOND_CR_FOUND,SECOND_LF_FOUND} httpd_purge_state_t;
static uint32_t now;static unsigned at,step;static const char *input;
static uint32_t mico_rtos_get_time(void){return now;}
static int httpd_recv(int fd,unsigned char *p,unsigned n,int flags){(void)fd;(void)n;(void)flags;now+=step;if(input){if(!input[at])return 0;*p=input[at++];}else{*p='x';at++;}return 1;}
'''+code+r'''
int main(void){
 input="Host: device\r\n\r\n";assert(!httpd_purge_headers(1));
 at=0;input=NULL;assert(httpd_purge_headers(1)<0&&at==4097);
 at=0;step=1000;now=UINT32_MAX-999;assert(httpd_purge_headers(1)<0&&at==5);
 puts("TC1 purged headers: normal CRLF, 4096-byte and timer-wrap bounds: OK");}
''')

if __name__=='__main__':unittest.main()

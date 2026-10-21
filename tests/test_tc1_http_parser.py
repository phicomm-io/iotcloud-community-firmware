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

if __name__=='__main__':unittest.main()

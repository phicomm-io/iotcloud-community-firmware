#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "common/httpd/product.h"
static long budget=-1,blocks=0;
static unsigned claim_result=600,claims,releases,waiting,enters,leaves;
static const void *borrowed;static size_t borrowed_length;static ph_buf *reply;
void *ph_wire_malloc(size_t n){if(!budget)return NULL;if(budget>0)--budget;void *p=malloc(n);if(p)blocks++;return p;}
void ph_wire_free(void *p){if(p){blocks--;free(p);}}
unsigned service_memory_claim(size_t n){claims++;assert(n<=20000);return claim_result;}
void service_memory_release(unsigned n){assert(n==600);releases++;}
void service_memory_waiting(bool enter){if(enter){waiting++;enters++;}else{assert(waiting);waiting--;leaves++;}}
ph_buf *service_command_mp(const void *p,size_t n){borrowed=p;borrowed_length=n;ph_buf *out=reply;reply=NULL;return out;}
ph_buf *service_query_mp(const char *action){assert(!strcmp(action,"status.get"));ph_buf *out=reply;reply=NULL;return out;}
static void message(ph_http_exchange *x,const char *key,const char *text,unsigned status){
 assert(x->response.status==status&&!strcmp(x->response.type,"application/msgpack"));
 ph_value root,v;assert(phv_parse(1,x->response.body,x->response.length,&root));
 assert(phv_get(&root,key,&v)&&phv_equal_text(&v,text));
}
static ph_buf *response(void){ph_writer w;assert(phw_init(&w,1));phw_begin(&w,NULL,false);phw_text(&w,"code","ok");phw_end(&w);return phw_take(&w);}
static void reset(void){assert(!blocks&&!waiting);claims=releases=enters=leaves=0;claim_result=600;budget=-1;}
static void admission(void){
 reset();ph_http_product p;ph_http_exchange x={0};ph_http_product_init(&p,&x);message(&x,"code","busy",503);
 claim_result=0;assert(ph_http_claim(&p,&x,35,true)==PH_HTTP_WAIT);assert(waiting==1&&enters==1);
 assert(ph_http_claim(&p,&x,35,true)==PH_HTTP_WAIT&&enters==1);
 claim_result=600;assert(ph_http_claim(&p,&x,35,true)==PH_HTTP_READ_BODY);assert(!waiting&&leaves==1&&p.capacity==35&&blocks==1);
 unsigned char request[35];memset(request,0,sizeof(request));assert(ph_http_body_copy(&p,request,10));assert(ph_http_body_copy(&p,request+10,25));
 assert(!ph_http_body_copy(&p,request,1));assert(p.used==35&&!memcmp(p.body,request,35));
 ph_http_product_release(&p);assert(releases==1&&!blocks&&!p.body&&!p.claim);ph_http_product_release(&p);assert(releases==1);
 reset();ph_http_product_init(&p,&x);claim_result=0;
 assert(ph_http_claim(&p,&x,1025,true)==PH_HTTP_REPLY&&!waiting);message(&x,"code","busy",503);
 assert(ph_http_claim(&p,&x,20000,true)==PH_HTTP_REPLY&&!waiting);ph_http_product_release(&p);
 reset();ph_http_product_init(&p,&x);claim_result=0;assert(ph_http_claim(&p,&x,1,true)==PH_HTTP_WAIT);ph_http_product_release(&p);assert(!waiting&&leaves==1);
 reset();ph_http_product_init(&p,&x);assert(ph_http_claim(&p,&x,0,false)==PH_HTTP_REPLY&&!p.body);ph_http_product_release(&p);assert(releases==1);
}
static void failures(void){
 reset();ph_http_product p;ph_http_exchange x={0};ph_http_product_init(&p,&x);budget=0;
 assert(ph_http_claim(&p,&x,35,true)==PH_HTTP_REPLY&&p.claim==600&&!p.body);message(&x,"code","busy",503);
 ph_http_product_release(&p);assert(releases==1&&!blocks);budget=-1;
 for(unsigned limit=0;limit<5;limit++){
  ph_http_product_init(&p,&x);budget=limit;ph_writer w;ph_buf *wire=NULL;
  if(phw_init(&w,1)){phw_begin(&w,NULL,false);phw_text(&w,"code","ok");phw_end(&w);wire=phw_take(&w);}else phw_discard(&w);
  ph_http_wire_take(&p,&x,wire);if(wire)message(&x,"code","ok",200);else message(&x,"code","busy",503);
  ph_http_product_release(&p);assert(!blocks);budget=-1;
 }
}
static void ownership(void){
 reset();ph_http_product p;ph_http_exchange x={0};ph_http_product_init(&p,&x);
 unsigned char input[]={0x81,0xa1,'x',0};reply=response();ph_buf *first=reply;
 ph_http_command_mp(&p,&x,input,sizeof(input));assert(borrowed==input&&borrowed_length==sizeof(input)&&p.wire==first&&x.response.body==first->buf);
 memset(input,0xff,sizeof(input));message(&x,"code","ok",200);long before=blocks;
 reply=response();ph_buf *second=reply;ph_http_query(&p,&x,"status.get");assert(p.wire==second&&blocks==before);message(&x,"code","ok",200);
 ph_http_command_mp(&p,&x,input,sizeof(input));assert(!p.wire&&!blocks);message(&x,"code","busy",503);ph_http_product_release(&p);
}
static void fixed(void){
 reset();ph_http_product p;ph_http_exchange x={0};ph_http_product_init(&p,&x);budget=0;
 ph_http_message(&p,&x,401,"password required");assert(x.response.body==p.scalar&&!blocks);message(&x,"message","password required",401);
 ph_http_message(&p,&x,503,"busy");message(&x,"message","busy",503);
 char huge[PH_HTTP_SCALAR_BYTES+1];memset(huge,'x',sizeof(huge)-1);huge[sizeof(huge)-1]=0;
 ph_http_message(&p,&x,400,huge);message(&x,"code","busy",503);assert(!blocks);
 const char invalid[]={ (char)0xc0,(char)0xaf,0};ph_http_message(&p,&x,400,invalid);message(&x,"code","busy",503);
 ph_http_product_release(&p);budget=-1;assert(!blocks);
}
int main(int argc,char **argv){assert(argc==2);if(!strcmp(argv[1],"admission"))admission();else if(!strcmp(argv[1],"failures"))failures();else if(!strcmp(argv[1],"ownership"))ownership();else if(!strcmp(argv[1],"fixed"))fixed();else abort();return 0;}

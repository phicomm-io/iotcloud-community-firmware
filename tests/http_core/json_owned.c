/* Execute the real HTTP owned JSON serializer with the pinned SDK json-c. */
#include "common/httpd/product.h"
#include "common/json_owned.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned allocation, fail_at;
int json_heap_permit(size_t n){(void)n;return !fail_at || ++allocation!=fail_at;}
unsigned service_json_claim(void){return 1;}
void service_json_release(unsigned token){(void)token;}
bool service_json_scope_begin(unsigned token,void *owner){(void)token;(void)owner;return false;}
unsigned service_json_scope_failed(unsigned token){(void)token;return 4;}
unsigned service_json_scope_end(unsigned token){(void)token;return 4;}
unsigned service_memory_claim(size_t n){return (unsigned)n+1;}
void service_memory_release(unsigned n){(void)n;}
void service_memory_waiting(bool on){(void)on;}
uint64_t service_memory_failures(void){return 0;}
json_object *service_parse(const void *p,size_t n){(void)n;return json_tokener_parse(p);}
static void empty(void){json_heap_stats_t s;json_heap_snapshot(&s);assert(!s.live_blocks);}
static json_object *fixture(void){
 json_object *o=json_object_new_object(),*a=json_object_new_array(),*child=json_object_new_object();
 assert(o&&a&&child);
 json_build_object_add(o,"quote\" slash/ tab\t",json_object_new_string("\b\f\n\r\t\\\"/\001 UTF-8 中文"));
 json_build_object_add(o,"integer",json_object_new_int64(-9223372036854775807LL));
 json_build_object_add(o,"double",json_object_new_double(12.125));
 json_build_object_add(o,"boolean",json_object_new_boolean(true));
 char controls[32];for(unsigned i=1;i<32;i++)controls[i-1]=(char)i;controls[31]=0;
 json_build_object_add(o,controls,json_object_new_string(controls));
 json_object_object_add(o,"null",NULL);
 json_build_object_add(child,"empty",json_object_new_object());
 json_build_array_add(a,json_object_new_string("value"));
 json_build_array_add(a,child);json_object_array_add(a,NULL);
 json_build_object_add(o,"array",a);json_build_object_add(o,"empty_array",json_object_new_array());
 return o;
}
static struct printbuf *golden(void){json_object *o=fixture();struct printbuf *p=json_object_to_json_string_ex(o);assert(p);json_object_put(o);return p;}
static unsigned published;
static int publish_result;
static const char *publish_expected;
static unsigned publish_baseline;
static int publish_owned(const char *topic,const char *body,bool retain){
 assert(!strcmp(topic,"test/state")&&retain&&!strcmp(body,publish_expected));
 json_heap_stats_t s;json_heap_snapshot(&s);assert(s.live_blocks==publish_baseline+2);
 published++;return publish_result;
}
static void parity(json_object *o,const char *expected){
 ph_http_product p={0};ph_http_exchange x={0};ph_http_json(&p,&x,o);
 assert(p.wire&&x.response.status==200&&!strcmp(x.response.body,expected));
 assert(x.response.length==strlen(expected));ph_http_product_release(&p);
}
int main(void){
 struct printbuf *expected=golden();
 parity(fixture(),expected->buf);
 /* Only owned wire bytes cross synchronous transport/reentrant callback waits. */
 json_heap_stats_t baseline;json_heap_snapshot(&baseline);publish_baseline=baseline.live_blocks;
 publish_expected=expected->buf;publish_result=-7;
 assert(json_owned_publish(publish_owned,"test/state",fixture(),true)==-7);
 assert(published==1);
 for(unsigned i=1;i<1000;i++){
  json_object *message=fixture();unsigned before=published;
  allocation=0;fail_at=i;
  int rc=json_owned_publish(publish_owned,"test/state",message,true);fail_at=0;
  if(rc==-7){assert(published==before+1);break;}
  assert(rc==-1&&published==before);
 }
 /* A shared root and every descendant remain unchanged. */
 json_object *root=fixture();parity(json_object_get(root),expected->buf);
 assert(!strcmp(json_object_to_json_string(root),expected->buf));json_object_put(root);
 /* A shared parent forbids mutation of its otherwise unique children. */
 json_object *alias=fixture(),*outer=json_object_new_object();
 json_build_object_add(outer,"alias",json_object_get(alias));
 struct printbuf *shared=json_object_to_json_string_ex(outer);assert(shared);
 parity(outer,shared->buf);assert(!strcmp(json_object_to_json_string(alias),expected->buf));
 printbuf_free(shared);json_object_put(alias);
 /* Shared arrays and aliased members inside an exclusively owned graph. */
 json_object *a=json_object_new_array(),*leaf=json_object_new_object();
 json_build_object_add(leaf,"key",json_object_new_string("untouched"));
 json_build_array_add(a,json_object_get(leaf));json_build_array_add(a,json_object_get(leaf));
 shared=json_object_to_json_string_ex(a);assert(shared);parity(a,shared->buf);
 assert(json_object_object_get(leaf,"key"));printbuf_free(shared);json_object_put(leaf);
 /* Every allocation failure during serialization is atomic at the HTTP API. */
 unsigned failures=0;
 for(unsigned i=1;i<1000;i++){
  root=fixture();ph_http_product p={0};ph_http_exchange x={0};
  allocation=0;fail_at=i;ph_http_json(&p,&x,root);fail_at=0;
  if(p.wire){assert(!strcmp(p.wire->buf,expected->buf));ph_http_product_release(&p);break;}
  assert(x.response.status==503);assert(!strcmp(x.response.body,"{\"code\":\"busy\"}"));
  ph_http_product_release(&p);failures++;
 }
 assert(failures>10&&failures<999);
 /* Failed encodes of a shared root must preserve every child too. */
 for(unsigned i=1;i<=failures;i++){
  root=fixture();ph_http_product shared_product={0};ph_http_exchange shared_x={0};
  allocation=0;fail_at=i;ph_http_json(&shared_product,&shared_x,json_object_get(root));fail_at=0;
  assert(!strcmp(json_object_to_json_string(root),expected->buf));
  ph_http_product_release(&shared_product);json_object_put(root);
 }
 printbuf_free(expected);empty();
 /* Failed builders never publish a partial object, including a failed child. */
 for(unsigned child=0;child<2;child++){
  root=json_object_new_object();json_object *bad=child?json_object_new_object():root;
  json_build_object_add(bad,"required",NULL);
  if(child)json_build_object_add(root,"child",bad);
  ph_http_product p={0};ph_http_exchange x={0};ph_http_json(&p,&x,root);
  assert(!p.wire&&x.response.status==503);ph_http_product_release(&p);empty();
 }
 /* Depth is bounded even for a graph cycle. Retain an external root so that
  * unlinking the deliberately cyclic SDK fixture cannot destroy its table. */
 root=json_object_new_object();json_build_object_add(root,"self",json_object_get(root));
 ph_http_product p={0};ph_http_exchange x={0};ph_http_json(&p,&x,json_object_get(root));
 assert(!p.wire&&x.response.status==503);json_object_object_del(root,"self");json_object_put(root);empty();
 for(unsigned depth=31;depth<=32;depth++){
  root=json_object_new_int(1);
  for(unsigned i=0;i<depth;i++){json_object *next=json_object_new_array();json_build_array_add(next,root);root=next;}
  memset(&p,0,sizeof(p));memset(&x,0,sizeof(x));ph_http_json(&p,&x,root);
  assert((p.wire!=NULL)==(depth==31));ph_http_product_release(&p);empty();
 }
 /* A rejected encode cannot poison the next healthy request. */
 expected=golden();parity(fixture(),expected->buf);printbuf_free(expected);empty();
 printf("owned JSON: byte parity, aliases, %u OOM points, failed graphs, cycles/depth and recovery PASS\n",failures);
 return 0;
}

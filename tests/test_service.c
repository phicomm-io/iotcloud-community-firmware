#include "mico.h"
#include "../common/service.h"
#include <assert.h>
#include <errno.h>
#include <string.h>
json_object *automation_command(const char *a,json_object *p,const char **c,bool *h){(void)a;(void)p;(void)c;*h=false;return NULL;}
uint32_t test_now;
static unsigned mutations;
json_object *service_product_capabilities(void){return json_object_new_object();}
json_object *service_product(const char *a,json_object *p,const char **code){(void)p;*code="ok";if(!strcmp(a,"fan.set"))mutations++;else if(strcmp(a,"status.get"))*code="unsupported";return json_object_new_object();}
static json_object *run(const char *s,const char *expected){json_object *q=service_parse(s,strlen(s)),*r=service_command(q);if(q)json_object_put(q);assert(r);if(strcmp(json_object_get_string(json_object_object_get(r,"code")),expected)){fprintf(stderr,"request=%s expected=%s got=%s\n",s,expected,json_object_to_json_string(r));abort();}return r;}
#define CHECK(s,c) do {json_object *r=run(s,c);json_object_put(r);}while(0)
int main(void){
 service_init();
 CHECK("{\"action\":\"status.get\",\"params\":{\"view\":\"live\\u0000full\"}}","invalid_params");
 CHECK("{\"action\":\"status.get\",\"params\":{}}","ok");
 CHECK("{\"action\":\"fan.set\",\"request_id\":\"one\",\"params\":{\"on\":true}}","ok");
 CHECK("{\"action\":\"fan.set\",\"request_id\":\"one\",\"params\":{\"on\":true}}","ok");assert(mutations==1);
 CHECK("{\"action\":\"fan.set\",\"request_id\":\"one\",\"params\":{\"on\":false}}","conflict");assert(mutations==1);
 CHECK("{\"action\":\"fan.set\",\"params\":null}","invalid_params");
 CHECK("{\"action\":\"fan.set\",\"params\":{},\"extra\":true}","invalid_params");
 CHECK("{\"action\":\"fan.set\",\"params\":{}} junk","invalid_params");
 CHECK("{\"action\":\"status.get\\u0000set\",\"params\":{}}","invalid_params");
 errno=ERANGE;
 CHECK("{\"action\":\"logs.get\",\"params\":{\"limit\":8}}","ok");
 CHECK("{\"action\":\"logs.get\",\"params\":{\"limit\":9}}","invalid_params");
 for(unsigned i=0;i<70;i++)service_event("info","test","transition");
 service_event("info","test","password=secret");
 json_object *r=run("{\"action\":\"logs.get\",\"params\":{\"after\":1,\"limit\":8}}","ok");
 json_object *d=json_object_object_get(r,"data"),*a=json_object_object_get(d,"records");assert(json_object_get_boolean(json_object_object_get(d,"overwritten")));assert(json_object_array_length(a)==8);assert(!strstr(json_object_to_json_string(r),"secret"));json_object_put(r);
 r=run("{\"action\":\"logs.get\",\"params\":{}}","ok");
 d=json_object_object_get(r,"data");unsigned latest=json_object_get_int(json_object_object_get(d,"latest"));json_object_put(r);
 service_event("warn","mqtt","receive_error_-2147483648");
 service_event("warn","mqtt","subscribe_error_2147483647");
 service_event("info","test","unregistered_event");
 char request[160];snprintf(request,sizeof request,"{\"action\":\"logs.get\",\"params\":{\"after\":%u}}",latest);
 r=run(request,"ok");d=json_object_object_get(r,"data");a=json_object_object_get(d,"records");assert(json_object_array_length(a)==2);
 assert(!strcmp(json_object_get_string(json_object_object_get(json_object_array_get_idx(a,0),"event")),"receive_error_-2147483648"));
 assert(!strcmp(json_object_get_string(json_object_object_get(json_object_array_get_idx(a,1),"event")),"subscribe_error_2147483647"));json_object_put(r);
 puts("PASS shared service: envelope, replay, conflict, bounded paging and redaction");
}

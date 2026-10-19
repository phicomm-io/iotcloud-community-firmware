#include "mico.h"
#include "../common/service.h"
#include "json_heap.h"
static unsigned allocation, fail_at, mutations;
static bool fail_result;
static unsigned serialization, fail_serialization;
static const char *serialize(json_object *o) {
    if (++serialization == fail_serialization) return NULL;
    return json_object_to_json_string(o);
}
static json_object *result_object(void) {
    if (fail_result) { fail_result=false;return NULL; }
    return json_object_new_object();
}
static char *receipt_copy(const char *s) {
    if (++allocation == fail_at) return NULL;
    return json_heap_strdup(s);
}
#define json_object_to_json_string serialize
#define json_heap_strdup receipt_copy
#define json_object_new_object result_object
#include "../common/service.c"
#undef json_object_to_json_string
#undef json_heap_strdup
#undef json_object_new_object
uint32_t test_now;
json_object *automation_command(const char *a,json_object *p,const char **c,bool *h)
{ (void)a;(void)p;(void)c;*h=false;return NULL; }
json_object *service_product_capabilities(void) { return json_object_new_object(); }
json_object *service_product(const char *a,json_object *p,const char **c)
{ (void)a;(void)p;mutations++;*c="accepted";return NULL; }
static void command(const char *id, const char *code) {
    char text[128];
    snprintf(text,sizeof(text),"{\"action\":\"fan.set\",\"request_id\":\"%s\",\"params\":{}}",id);
    json_object *q=service_parse(text,strlen(text)),*r=service_command(q);
    if (!r || strcmp(json_object_get_string(json_object_object_get(r,"code")),code)) fprintf(stderr,"id=%s expected=%s got=%s mutations=%u\n",id,code,r?json_object_to_json_string(r):"NULL",mutations);
    assert(r && !strcmp(json_object_get_string(json_object_object_get(r,"code")),code));
    json_object_put(q);json_object_put(r);
}
int main(void) {
    service_init();
    fail_serialization=1;command("before","busy");assert(!mutations);
    fail_serialization=0;allocation=0;fail_at=1;command("after","accepted");assert(mutations==1);
    fail_at=0;command("after","busy");assert(mutations==1);
    command("before","accepted");assert(mutations==2);
    command("before","accepted");assert(mutations==2);
    for(unsigned i=0;i<100;i++) { char id[32];snprintf(id,sizeof(id),"evict%u",i);command(id,"accepted"); }
    assert(mutations==102);
    json_object *q=service_parse("{\"action\":\"fan.set\",\"request_id\":\"result-oom\",\"params\":{}}",strlen("{\"action\":\"fan.set\",\"request_id\":\"result-oom\",\"params\":{}}"));
    assert(q);fail_result=true;
    assert(!service_command(q));json_object_put(q);assert(mutations==103);
    command("result-oom","busy");assert(mutations==103);
    serialization=0;fail_serialization=1;
    command("serialize-before","busy");assert(mutations==103);
    serialization=0;fail_serialization=2;
    command("serialize-after","accepted");assert(mutations==104);
    fail_serialization=0;
    command("serialize-after","busy");assert(mutations==104);
    puts("Receipt allocation failure preserves at-most-once execution; bounded eviction passed");
}

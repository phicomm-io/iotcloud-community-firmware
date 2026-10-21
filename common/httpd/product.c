/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "product.h"
#include <string.h>

void ph_http_busy(ph_http_exchange *x)
{
    static const char busy[]="{\"code\":\"busy\"}";
    x->response.status=503;x->response.type="application/json";
    x->response.body=busy;x->response.length=sizeof(busy)-1;
}
void ph_http_product_init(ph_http_product *p,ph_http_exchange *x)
{ memset(p,0,sizeof(*p));ph_http_busy(x); }
void ph_http_json(ph_http_product *p,ph_http_exchange *x,json_object *o)
{
    p->wire=o?json_object_to_json_string_ex(o):NULL;
    if(o)json_object_put(o);
    if(!p->wire) { ph_http_busy(x);return; }
    x->response.status=200;x->response.type="application/json";
    x->response.body=p->wire->buf;x->response.length=p->wire->bpos;
}
void ph_http_message(ph_http_product *p,ph_http_exchange *x,unsigned status,const char *text)
{
    json_object *o=json_object_new_object();
    if(o)json_build_object_add(o,"message",json_object_new_string(text));
    ph_http_json(p,x,o);if(p->wire)x->response.status=status;
}
ph_http_action ph_http_claim(ph_http_product *p,ph_http_exchange *x,size_t input,bool read_body)
{
    if(!p->claim)p->claim=service_memory_claim(input);
    if(!p->claim) {
        if(service_memory_cost(input) && input<=1024) {
            if(!p->waiting) { service_memory_waiting(true);p->waiting=true; }
            return PH_HTTP_WAIT;
        }
        ph_http_busy(x);return PH_HTTP_REPLY;
    }
    if(p->waiting) { service_memory_waiting(false);p->waiting=false; }
    p->failures=service_memory_failures();
    if(read_body && input) {
        p->capacity=input;
        p->body=json_heap_malloc(input+1);
        if(!p->body) { ph_http_busy(x);return PH_HTTP_REPLY; }
    }
    return read_body?PH_HTTP_READ_BODY:PH_HTTP_REPLY;
}
bool ph_http_body_copy(ph_http_product *p,const void *data,size_t n)
{
    if(!p->body || n>p->capacity-p->used)return false;
    memcpy(p->body+p->used,data,n);p->used+=n;return true;
}
json_object *ph_http_body_json(ph_http_product *p,ph_http_exchange *x)
{
    json_object *o=NULL;
    if(p->body && p->used==x->request->length && !memchr(p->body,0,p->used)) {
        p->body[p->used]=0;
        o=service_parse(p->body,p->used);
    }
    if(p->body)json_heap_free(p->body);
    p->body=NULL;
    if(o && !json_object_is_type(o,json_type_object)) { json_object_put(o);o=NULL; }
    return o;
}
void ph_http_product_release(ph_http_product *p)
{
    if(p->waiting)service_memory_waiting(false);
    if(p->body)json_heap_free(p->body);
    if(p->wire)printbuf_free(p->wire);
    if(p->claim)service_memory_release(p->claim);
    memset(p,0,sizeof(*p));
}

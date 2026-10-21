/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "product.h"
#include "../json_owned.h"
#include <string.h>
#include <stdio.h>


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
    p->wire=json_owned_to_wire(o);
    service_json_release(p->json_claim);p->json_claim=0;
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
    if(!p->json_claim)p->json_claim=service_json_claim();
    if(p->json_claim && !p->claim)p->claim=service_memory_claim(input);
    if(!p->claim) {
        service_json_release(p->json_claim);p->json_claim=0;
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
    service_json_release(p->json_claim);
    memset(p,0,sizeof(*p));
}

size_t ph_http_diag_append(char *body,size_t capacity,size_t used,const ph_http_diagnostics *d)
{
    if(used<2 || used>=capacity || memcmp(body+used-2,"}}",2))return 0;
    int n=snprintf(body+used-2,capacity-used+2,
        ",\"http_core_owner_state\":%u,\"http_core_restarts\":%u,"
        "\"http_core_close_failures\":%lu,\"http_core_last_close_rc\":%d,\"http_core_last_close_errno\":%d,"
        "\"http_core_slot_bytes\":%lu,\"http_core_server_bytes\":%lu,"
        "\"http_core_last_select_rc\":%d,\"http_core_last_read_rc\":%d,"
        "\"http_core_occupied\":%lu,\"http_tcp_sample_ms\":%lu,"
        "\"http_core_accepted\":%lu,\"http_core_rejected\":%lu,"
        "\"http_core_tcp_limit\":%d,\"http_core_tcp_connections\":%d,\"http_core_tcp_rc\":%d,"
        "\"http_core_read_errno\":%d,\"http_core_closed\":[%lu,%lu,%lu,%lu,%lu,%lu,%lu]}}",
        d->owner_state,d->restarts,(unsigned long)d->close_failures,d->last_close_rc,d->last_close_errno,
        (unsigned long)d->slot_bytes,(unsigned long)d->server_bytes,d->last_select_rc,d->last_read_rc,(unsigned long)d->occupied,(unsigned long)d->connections_sample_ms,
        (unsigned long)d->accepted,(unsigned long)d->rejected,d->limit,d->connections,d->connections_get_rc,d->read_errno,
        (unsigned long)d->closed[0],(unsigned long)d->closed[1],(unsigned long)d->closed[2],
        (unsigned long)d->closed[3],(unsigned long)d->closed[4],(unsigned long)d->closed[5],(unsigned long)d->closed[6]);
    return n>0 && (size_t)n<capacity-used+2?used-2+(size_t)n:0;
}

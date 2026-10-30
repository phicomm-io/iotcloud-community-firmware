/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "product.h"
#include <stdlib.h>
#include <string.h>
void ph_http_busy(ph_http_exchange *x)
{
 static const unsigned char busy[]={0x81,0xa4,'c','o','d','e',0xa4,'b','u','s','y'};
 x->response.status=503;x->response.type="application/msgpack";
 x->response.body=busy;x->response.length=sizeof(busy);
}
void ph_http_product_init(ph_http_product *p,ph_http_exchange *x){memset(p,0,sizeof(*p));ph_http_busy(x);}
void ph_http_wire_take(ph_http_product *p,ph_http_exchange *x,ph_buf *wire)
{
 ph_buf_free(p->wire);p->wire=wire;if(!wire){ph_http_busy(x);return;}
 x->response.status=200;x->response.type="application/msgpack";
 x->response.body=wire->buf;x->response.length=wire->len;
}
void ph_http_command_mp(ph_http_product *p,ph_http_exchange *x,const void *request,size_t length)
{ph_http_wire_take(p,x,service_command_mp(request,length));}
void ph_http_query(ph_http_product *p,ph_http_exchange *x,const char *action)
{
 ph_http_wire_take(p,x,service_query_mp(action));
 /* Small error envelope only: normal cached status never incurs a decode.
  * Return HTTP 503 for a temporary service refusal. */
 if(p->wire&&p->wire->len<128){ph_value root,code;
  if(phv_parse(PH_WIRE_MSGPACK,p->wire->buf,p->wire->len,&root)&&phv_get(&root,"code",&code)&&phv_equal_text(&code,"busy"))x->response.status=503;
 }
}
void ph_http_message(ph_http_product *p,ph_http_exchange *x,unsigned status,const char *text)
{
 ph_writer w;ph_buf storage;
 if(!phw_init_fixed(&w,PH_WIRE_MSGPACK,&storage,p->scalar,sizeof(p->scalar))){ph_http_busy(x);return;}
 phw_begin(&w,NULL,false);phw_text(&w,"message",text);phw_end(&w);size_t length=phw_finish_fixed(&w);
 if(!length){ph_http_busy(x);return;}
 x->response.status=status;x->response.type="application/msgpack";x->response.body=p->scalar;x->response.length=length;
}
ph_http_action ph_http_claim(ph_http_product *p,ph_http_exchange *x,size_t input,bool read_body)
{
 if(!p->claim)p->claim=service_memory_claim(input);
 if(!p->claim){
  if(service_memory_cost(input)&&input<=1024){if(!p->waiting){service_memory_waiting(true);p->waiting=true;}return PH_HTTP_WAIT;}
  ph_http_busy(x);return PH_HTTP_REPLY;
 }
 if(p->waiting){service_memory_waiting(false);p->waiting=false;}
 if(read_body&&input){p->capacity=input;p->body=ph_wire_malloc(input+1);if(!p->body){ph_http_busy(x);return PH_HTTP_REPLY;}}
 return read_body?PH_HTTP_READ_BODY:PH_HTTP_REPLY;
}
bool ph_http_body_copy(ph_http_product *p,const void *data,size_t n)
{if(!p->body||p->used>p->capacity||n>p->capacity-p->used)return false;memcpy(p->body+p->used,data,n);p->used+=n;return true;}
void ph_http_product_release(ph_http_product *p)
{
 if(p->waiting)service_memory_waiting(false);ph_wire_free(p->body);ph_buf_free(p->wire);
 if(p->claim)service_memory_release(p->claim);memset(p,0,sizeof(*p));
}

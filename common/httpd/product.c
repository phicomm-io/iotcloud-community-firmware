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
static void ph_http_service_status(ph_http_product *p,ph_http_exchange *x,bool query)
{
 /* Small error envelope only: normal cached status never incurs a decode.
  * Revocation after admission must still return an HTTP auth failure. */
 if(p->wire&&p->wire->len<128){ph_value root,code;
  if(phv_parse(PH_WIRE_MSGPACK,p->wire->buf,p->wire->len,&root)&&phv_get(&root,"code",&code)){
   if(phv_equal_text(&code,"unauthorized"))x->response.status=401;
   else if(query&&phv_equal_text(&code,"busy"))x->response.status=503;
  }
 }
}
void ph_http_command_mp(ph_http_product *p,ph_http_exchange *x,const void *request,size_t length)
{
 ph_http_wire_take(p,x,service_http_command_mp(request,length,&p->auth));
 ph_http_service_status(p,x,false);
}
void ph_http_query(ph_http_product *p,ph_http_exchange *x,const char *action)
{
 ph_http_wire_take(p,x,service_http_query_mp(action,&p->auth));
 ph_http_service_status(p,x,true);
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
#if defined(PHICOMM_STATUS_MODEL) && PHICOMM_STATUS_MODEL == 2
ph_http_action ph_http_control_claim(ph_http_product *p,ph_http_exchange *x,size_t input)
{
 if(!input||input>SERVICE_HTTP_CONTROL_INPUT)return ph_http_claim(p,x,input,true);
 ph_http_action next=ph_http_claim(p,x,input,true);
 if(next==PH_HTTP_WAIT)return next;
 if(p->body && (p->control_credit || (p->control_credit=service_http_control_credit())))return PH_HTTP_READ_BODY;
 /* Neither body admission nor credit preparation executes business. Release
  * partial storage and retry under the core's original, non-renewing deadline. */
 ph_wire_free(p->body);p->body=NULL;p->capacity=p->used=0;
 service_http_control_credit_free(p->control_credit);p->control_credit=NULL;
 if(p->claim){service_memory_release(p->claim);p->claim=0;}
 if(!p->waiting){service_memory_waiting(true);p->waiting=true;}
 return PH_HTTP_WAIT;
}
bool ph_http_control_command(ph_http_product *p,ph_http_exchange *x)
{
 size_t length=0;unsigned status=200;
 bool handled=service_http_control_mp(p->body,p->used,&p->auth,&p->control_credit,
                                     p->scalar,sizeof(p->scalar),&length,&status);
 /* Non-control reads/writes must not retain unused credit in their ordinary
  * dispatcher. Completion never retries a possibly executed command. */
 service_http_control_credit_free(p->control_credit);p->control_credit=NULL;
 if(!handled)return false;
 x->response.status=status;x->response.type="application/msgpack";
 x->response.body=p->scalar;x->response.length=length;return true;
}
#endif
void ph_http_product_release(ph_http_product *p)
{
#if defined(PHICOMM_STATUS_MODEL) && PHICOMM_STATUS_MODEL == 2
 service_http_control_credit_free(p->control_credit);
#endif
 if(p->waiting)service_memory_waiting(false);ph_wire_free(p->body);ph_buf_free(p->wire);
 if(p->claim)service_memory_release(p->claim);memset(p,0,sizeof(*p));
}

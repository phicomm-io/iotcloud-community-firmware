/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "port_internal.h"
#include "port.h"
#include "../protocol/httpd/http.h"
#include "../protocol/http_cors.h"
#include "web.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef PH_HTTP_PORT
#define PH_HTTP_PORT 80
#endif
/* The shared HTTP core grants one response/body lease across all idle sockets.
 * These bounded buffers live until release(), including partial sends. */
static unsigned char input[PH_INPUT_MAX],output[PH_OUTPUT_MAX];
static size_t used;
static ph_wire_format format;
static bool authenticated,admin;
static bool route_is(const ph_http_request *r,const char *path) {
 size_t n=strcspn(r->path,"?");return strlen(path)==n&&!memcmp(r->path,path,n);
}
size_t bl_error(void *out,size_t cap,ph_wire_format f,const char *code) {
 ph_writer w;struct printbuf b;phw_init_fixed(&w,f,&b,out,cap);phw_begin(&w,NULL,false);
 phw_text(&w,"code",code);phw_text(&w,"message",code);phw_begin(&w,"data",false);phw_end(&w);phw_end(&w);
 return phw_finish_fixed(&w);
}
static void response(ph_http_exchange *x,unsigned status,size_t n) {
 x->response.status=status;x->response.type=ph_wire_mime(format);x->response.body=output;x->response.length=n;
}
static ph_http_action fail(ph_http_exchange *x,unsigned status,const char *code) {
 response(x,status,bl_error(output,sizeof(output),format,code));return PH_HTTP_REPLY;
}
static bool local_origin(const char *origin) {
 char own[80];if(PH_HTTP_PORT==80)snprintf(own,sizeof(own),"http://%s",HAL_GetMyIPString());
 else snprintf(own,sizeof(own),"http://%s:%u",HAL_GetMyIPString(),(unsigned)PH_HTTP_PORT);
 size_t n=strlen(own);
 return !*origin||!strcmp(origin,own)||(!strncmp(origin,own,n)&&!strcmp(origin+n,":80"))||
  !strcmp(origin,"http://192.168.0.1")||*phicomm_cors_headers(origin);
}
static bool authenticate(const char *authorization,bool *is_admin) {
 bool ok;ph_auth_context ctx;
 bl_lock();
 if(ph_http_basic(authorization,"admin",CFG_GetWebPassword())){ph_auth_internal_context(&bl_auth,&ctx);ok=true;}
 else ok=ph_auth_http_check(&bl_auth,authorization,&ctx)&&ph_auth_context_valid(&ctx,PH_AUTH_SCOPE_CONTROL);
 *is_admin=ok&&ph_auth_context_valid(&ctx,PH_AUTH_SCOPE_ADMIN);bl_unlock();return ok;
}
static ph_http_action begin(ph_http_exchange *x) {
 const ph_http_request *r=x->request;used=0;authenticated=admin=false;
 format=r->have_accept==1&&ph_binary_format(r->accept_format)?r->accept_format:
  r->have_type==1&&ph_binary_format(r->content_format)?r->content_format:PH_WIRE_MSGPACK;
 x->response.cors=phicomm_cors_headers(r->origin);
 if(!strcmp(r->method,"OPTIONS")) {
  if(!*x->response.cors)return fail(x,403,"forbidden_origin");
  return fail(x,200,"ok");
 }
 if(!local_origin(r->origin))return fail(x,403,"forbidden_origin");
 if(!bl_ready)return fail(x,503,"not_ready");
 authenticated=authenticate(r->authorization,&admin);
 bool home=route_is(r,"/")||route_is(r,"/index.html");
 if(home) {
  if(strcmp(r->method,"GET")&&strcmp(r->method,"HEAD"))return fail(x,405,"method_not_allowed");
  if(!authenticated){x->response.realm="iotcloud";return fail(x,401,"unauthorized");}
  x->response.status=200;x->response.type="text/html; charset=utf-8";x->response.body=ph_web;x->response.length=sizeof(ph_web);
  x->response.gzip=true;x->response.suppress_body=!strcmp(r->method,"HEAD");return PH_HTTP_REPLY;
 }
 if(strncmp(r->path,"/api/v1/",8))return fail(x,404,"not_found");
 if(r->have_accept && (r->have_accept!=1||!ph_binary_format(r->accept_format)))return fail(x,406,"not_acceptable");
 if(route_is(r,"/api/v1/auth/status")&&!strcmp(r->method,"GET")) {
  ph_writer w;struct printbuf b;phw_init_fixed(&w,format,&b,output,sizeof(output));phw_begin(&w,NULL,false);
  phw_bool(&w,"password_required",*CFG_GetWebPassword()!=0);phw_bool(&w,"authenticated",authenticated);phw_end(&w);
  response(x,200,phw_finish_fixed(&w));return PH_HTTP_REPLY;
 }
 bool login=route_is(r,"/api/v1/auth/login");
 if(!authenticated&&!login){x->response.realm="iotcloud";return fail(x,401,"unauthorized");}
 if(!strcmp(r->method,"GET")) {
  const char *action=route_is(r,"/api/v1/info")?"info.get":route_is(r,"/api/v1/status")?"status.get":
   route_is(r,"/api/v1/capabilities")?"capabilities.get":route_is(r,"/api/v1/config")?"config.get":NULL;
  if(!action)return fail(x,404,"not_found");
  bl_lock();size_t n=ph_query(&bl_device,action,format,output,sizeof(output));bl_unlock();
  if(!n)return fail(x,503,"busy");response(x,200,n);return PH_HTTP_REPLY;
 }
 if(strcmp(r->method,"POST"))return fail(x,405,"method_not_allowed");
 if(!route_is(r,"/api/v1/command")&&!route_is(r,"/api/v1/auth/login")&&!route_is(r,"/api/v1/auth/logout"))return fail(x,404,"not_found");
 if(r->have_type!=1||!ph_binary_format(r->content_format))return fail(x,415,"unsupported_media_type");
 if(!r->have_length||!r->length||r->length>PH_INPUT_MAX)return fail(x,413,"invalid_length");
 x->body_idle_ms=2000;x->body_total_ms=5000;return PH_HTTP_READ_BODY;
}
static bool body(ph_http_exchange *x,const void *p,size_t n) {
 (void)x;if(n>sizeof(input)-used)return false;memcpy(input+used,p,n);used+=n;return true;
}
static void complete(ph_http_exchange *x) {
 const ph_http_request *r=x->request;
 if(used!=r->length){fail(x,400,"incomplete_body");return;}
 if(route_is(r,"/api/v1/command")) {
  /* Revalidate after receiving: password/grants may have changed meanwhile. */
  if(!authenticate(r->authorization,&admin)){fail(x,401,"unauthorized");return;}
  bl_lock();size_t n=ph_command(&bl_device,input,used,r->content_format,format,true,admin,output,sizeof(output));bl_unlock();
  if(!n)fail(x,400,"invalid_params");else response(x,200,n);return;
 }
 ph_value root,v;char password[33]={0},token[33]={0};bool ok=false;
 if(!phv_parse(r->content_format,input,used,&root)||root.kind!=PHV_MAP){fail(x,400,"invalid_params");return;}
 bl_lock();
 if(route_is(r,"/api/v1/auth/logout")) {
  ok=!root.count; if(ok)ph_auth_http_logout(&bl_auth,r->authorization);
 } else if(phv_keys(&root,"|password|")&&phv_get(&root,"password",&v)&&phv_text(&v,password,sizeof(password))) {
  /* Empty-password devices require no login and consume no session slots. */
  ok=!*CFG_GetWebPassword()||ph_auth_http_login(&bl_auth,password,token);
 }
 bl_unlock();memset(password,0,sizeof(password));
 if(!ok){fail(x,401,"unauthorized");return;}
 ph_writer w;struct printbuf b;phw_init_fixed(&w,format,&b,output,sizeof(output));phw_begin(&w,NULL,false);
 phw_text(&w,"token",token);phw_text(&w,"token_type","Bearer");phw_u32(&w,"expires_in",PH_AUTH_IDLE_MS/1000);phw_end(&w);
 response(x,200,phw_finish_fixed(&w));memset(token,0,sizeof(token));
}
static void release(ph_http_exchange *x,bool sent) {(void)x;(void)sent;memset(input,0,used);used=0;}
static unsigned admission(void) {
 if(!bl_ready)return 0;
 bl_lock();bool upgrading=bl_ota_pending;bl_unlock();
 return upgrading?2:PH_HTTP_CLIENT_SLOTS;
}
const ph_http_app bl_http_app={begin,NULL,body,complete,release,admission};

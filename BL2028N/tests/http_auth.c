/* Production S30 HTTP callbacks + device dispatcher; no ph_port persistence. */
#include "port_internal.h"
#include "../protocol/httpd/http.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef PH_AUTH_HTTP_STATELESS
#error Synchronize the reviewed common auth snapshot before this regression
#endif
extern const ph_http_app bl_http_app;
ph_device bl_device;
ph_auth_state bl_auth;
volatile bool bl_ready=true;
bool bl_ota_pending;
static char password[33];
static uint32_t tick=12345,random_value;
static bool fail_random,fail_commit;
static unsigned relay_calls,random_calls;
static pthread_mutex_t product_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rendezvous=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static bool paused,gate_owned,attempting;
static unsigned pause_at;
static __thread bool worker;
static __thread unsigned lock_number;
/* HTTP must preserve its own OPEN/Basic source, which the credential epoch
 * can revoke. Converting it to the trusted internal source would survive. */
void __wrap_ph_auth_internal_context(ph_auth_state *s,ph_auth_context *c){(void)s;(void)c;assert(!"HTTP converted to internal authorization");}
void bl_lock(void){
 if(worker&&++lock_number==pause_at){
  assert(!pthread_mutex_lock(&rendezvous));paused=true;assert(!pthread_cond_broadcast(&changed));
  while(!gate_owned)assert(!pthread_cond_wait(&changed,&rendezvous));
  assert(pthread_mutex_trylock(&product_lock)==EBUSY);attempting=true;
  assert(!pthread_cond_broadcast(&changed));assert(!pthread_mutex_unlock(&rendezvous));
 }
 assert(!pthread_mutex_lock(&product_lock));
}
void bl_unlock(void){assert(!pthread_mutex_unlock(&product_lock));}
const char *CFG_GetWebPassword(void){return password;}
int PH_BL_CopyLocalIPAddress(char *out,size_t n){if(n<10)return 0;strcpy(out,"127.0.0.1");return 1;}
static uint32_t now(void *u){(void)u;return tick;}
static bool random_bytes(void *u,uint8_t *p,size_t n){(void)u;random_calls++;if(fail_random)return false;memset(p,0,n);uint32_t v=++random_value;memcpy(p,&v,sizeof(v));return true;}
static bool get_password(void *u,char out[33]){(void)u;strcpy(out,password);return true;}
static bool commit(void *u,const ph_auth_store *store){(void)store;if(fail_commit)return false;memmove(password,u,strlen(u)+1);return true;}
static void credential(const char *p,bool force){assert(ph_auth_credential_commit(&bl_auth,p,force,commit,(void *)p));}
static bool save(void *u,const ph_settings *s){(void)u;return ph_settings_valid(s);}
static void relay(void *u,bool on){(void)u;(void)on;relay_calls++;}
static ph_http_request request;
static ph_http_exchange exchange;
static unsigned char request_bytes[PH_INPUT_MAX];
static size_t request_length;
static void setup(const char *p){
 ph_status_cache_release(&bl_device);strcpy(password,p);
 ph_auth_store store;ph_auth_store_empty(&store);
 ph_auth_ops auth_ops={.now=now,.random=random_bytes,.password=get_password};
 assert(ph_auth_init(&bl_auth,&auth_ops,&store));
 ph_device_ops ops={.now=now,.save=save,.relay=relay};
 assert(ph_device_init(&bl_device,&ops,NULL,"aabbccddeeff","http-auth-fixture",1));
 fail_commit=fail_random=false;request_length=0;
}
static void http_request(const char *method,const char *path,const char *authorization,ph_wire_format f){
 memset(&request,0,sizeof(request));memset(&exchange,0,sizeof(exchange));
 strcpy(request.method,method);strcpy(request.path,path);strcpy(request.authorization,authorization);
 request.have_accept=1;request.accept_format=f;request.have_type=1;request.content_format=f;
 request.have_length=true;request.length=(uint32_t)request_length;exchange.request=&request;
}
static void envelope(unsigned status,const char *code){
 ph_value root,v;assert(exchange.response.status==status);
 assert(phv_parse(request.accept_format,exchange.response.body,exchange.response.length,&root));
 assert(phv_get(&root,"code",&v)&&phv_equal_text(&v,code));
}
static void make_command(const char *action,const char *id,ph_wire_format f){
 ph_writer w;struct printbuf b;assert(phw_init_fixed(&w,f,&b,request_bytes,sizeof(request_bytes)));
 phw_begin(&w,NULL,false);phw_text(&w,"action",action);phw_text(&w,"request_id",id);phw_begin(&w,"params",false);
 if(!strcmp(action,"outlet.set"))phw_bool(&w,"on",!bl_device.relay);
 phw_end(&w);phw_end(&w);request_length=phw_finish_fixed(&w);assert(request_length);
}
static void post_body(void){
 assert(bl_http_app.begin(&exchange)==PH_HTTP_READ_BODY);
 size_t n=request_length/2;assert(bl_http_app.body(&exchange,request_bytes,n));
 assert(bl_http_app.body(&exchange,request_bytes+n,request_length-n));
}
static void authorization(unsigned mode,char out[64]){
 if(mode==0)out[0]=0;
 else if(mode==1)strcpy(out,"Basic YWRtaW46c2VjcmV0");
 else{char token[33];assert(ph_auth_http_login(&bl_auth,"secret",token));snprintf(out,64,"Bearer %s",token);}
}
enum{KEEP,NOOP,FAILED,RESTORE,CHANGE_BACK,REVOKE,LOGOUT,EXPIRE};
enum{COMMAND,RECEIPT,COMMAND_QUERY,GET_QUERY};
static unsigned current_kind;
static void *finish_waiting(void *u){(void)u;worker=true;lock_number=0;
 if(current_kind==GET_QUERY)assert(bl_http_app.begin(&exchange)==PH_HTTP_REPLY);
 else bl_http_app.complete(&exchange);
 worker=false;return NULL;
}
static void change(unsigned how,const char *header){
 switch(how){
 case KEEP:break;
 case NOOP:credential(password,false);break;
 case FAILED:fail_commit=true;assert(!ph_auth_credential_commit(&bl_auth,password,true,commit,password));fail_commit=false;break;
 case RESTORE:credential(password,true);break;
 case CHANGE_BACK:{char before[33];strcpy(before,password);credential("different",false);credential(before,false);break;}
 case REVOKE:ph_auth_local_revoke(&bl_auth,&bl_auth.store);break;
 case LOGOUT:assert(ph_auth_http_logout(&bl_auth,header));break;
 case EXPIRE:tick+=PH_AUTH_IDLE_MS;break;
 default:abort();
 }
}
static unsigned serial;
static void waiting_request(unsigned mode,unsigned kind,unsigned how,ph_wire_format f){
 setup(mode?"secret":"");char header[64],id[64];authorization(mode,header);
 snprintf(id,sizeof(id),"auth-%u",++serial);make_command(kind==COMMAND_QUERY?"status.get":"outlet.set",id,f);
 http_request("POST","/api/v1/command",header,f);
 if(kind==RECEIPT){post_body();bl_http_app.complete(&exchange);envelope(200,"accepted");bl_http_app.release(&exchange,true);}
 if(kind!=GET_QUERY)post_body();else http_request("GET","/api/v1/status",header,f);
 unsigned before=relay_calls,before_sequence=bl_device.sequence;
 current_kind=kind;pause_at=kind==GET_QUERY?2:1;paused=gate_owned=attempting=false;
 pthread_t thread;assert(!pthread_create(&thread,NULL,finish_waiting,NULL));
 assert(!pthread_mutex_lock(&rendezvous));while(!paused)assert(!pthread_cond_wait(&changed,&rendezvous));
 bl_lock();gate_owned=true;assert(!pthread_cond_broadcast(&changed));
 while(!attempting)assert(!pthread_cond_wait(&changed,&rendezvous));
 change(how,header);bl_unlock();assert(!pthread_mutex_unlock(&rendezvous));
 assert(!pthread_join(thread,NULL));pause_at=0;
 bool allowed=how==KEEP||how==NOOP||how==FAILED;
 envelope(allowed?200:401,allowed?(kind==COMMAND||kind==RECEIPT?"accepted":"ok"):"unauthorized");
 assert(relay_calls==before+(allowed&&kind==COMMAND));assert(bl_device.sequence==before_sequence+(allowed&&kind==COMMAND));
 bl_http_app.release(&exchange,true);
}
static void login(const char *p,ph_wire_format f,unsigned status,const char *code,char token[33]){
 ph_writer w;struct printbuf b;assert(phw_init_fixed(&w,f,&b,request_bytes,sizeof(request_bytes)));
 phw_begin(&w,NULL,false);phw_text(&w,"password",p);phw_end(&w);request_length=phw_finish_fixed(&w);
 http_request("POST","/api/v1/auth/login","",f);post_body();bl_http_app.complete(&exchange);envelope(status,code);
 if(status==200){ph_value root,data,v;assert(phv_parse(f,exchange.response.body,exchange.response.length,&root));
  assert(phv_get(&root,"data",&data)&&phv_get(&data,"token",&v)&&phv_text(&v,token,33));
  assert(phv_get(&data,"password_required",&v)&&v.kind==(token[0]?PHV_TRUE:PHV_FALSE));}
 bl_http_app.release(&exchange,true);
}
static void login_lifecycle(ph_wire_format f){
 setup("");unsigned before=random_calls;char tokens[4][33],extra[33];
 for(unsigned i=0;i<12;i++){login("",f,200,"ok",extra);assert(!*extra);}
 assert(random_calls==before);for(unsigned i=0;i<PH_AUTH_HTTP_SLOTS;i++)assert(!bl_auth.http[i].active);
 login("wrong",f,401,"unauthorized",extra);
 credential("secret",false);for(unsigned i=0;i<4;i++)login("secret",f,200,"ok",tokens[i]);
 login("secret",f,503,"busy",extra);login("wrong",f,401,"unauthorized",extra);
 for(unsigned i=0;i<4;i++){char header[64];snprintf(header,sizeof(header),"Bearer %s",tokens[i]);ph_auth_context c;assert(ph_auth_http_check(&bl_auth,header,&c));}
 const unsigned char map=0x80;request_bytes[0]=map;request_length=1;
 char header[64];snprintf(header,sizeof(header),"Bearer %s",tokens[0]);
 http_request("POST","/api/v1/auth/logout",header,PH_WIRE_MSGPACK);post_body();bl_http_app.complete(&exchange);envelope(200,"ok");bl_http_app.release(&exchange,true);
 ph_auth_context c;assert(!ph_auth_http_check(&bl_auth,header,&c));login("secret",f,200,"ok",extra);
 setup("secret");fail_random=true;login("secret",f,503,"busy",extra);fail_random=false;
 http_request("GET","/api/v1/status","Basic YWRtaW46d3Jvbmc=",f);assert(bl_http_app.begin(&exchange)==PH_HTTP_REPLY);envelope(401,"unauthorized");bl_http_app.release(&exchange,true);
}
static unsigned body_cases;
static void body_change(unsigned mode,unsigned how,ph_wire_format f){
 setup(mode?"secret":"");char header[64];authorization(mode,header);
 make_command("outlet.set","body-credential",f);http_request("POST","/api/v1/command",header,f);
 assert(bl_http_app.begin(&exchange)==PH_HTTP_READ_BODY);size_t n=request_length/2;
 assert(bl_http_app.body(&exchange,request_bytes,n));bl_lock();change(how,header);bl_unlock();
 unsigned before=relay_calls;assert(bl_http_app.body(&exchange,request_bytes+n,request_length-n));bl_http_app.complete(&exchange);
 bool allowed=how==NOOP||how==FAILED;envelope(allowed?200:401,allowed?"accepted":"unauthorized");assert(relay_calls==before+allowed);
 bl_http_app.release(&exchange,true);body_cases++;
}
int main(void){
 for(unsigned f=PH_WIRE_MSGPACK;f<=PH_WIRE_CBOR;f++){
  login_lifecycle((ph_wire_format)f);
  for(unsigned mode=0;mode<3;mode++)for(unsigned kind=COMMAND;kind<=GET_QUERY;kind++)
   for(unsigned how=KEEP;how<=REVOKE;how++)waiting_request(mode,kind,how,(ph_wire_format)f);
  for(unsigned kind=COMMAND;kind<=GET_QUERY;kind++)for(unsigned how=LOGOUT;how<=EXPIRE;how++)waiting_request(2,kind,how,(ph_wire_format)f);
  for(unsigned mode=0;mode<3;mode++)for(unsigned how=NOOP;how<=RESTORE;how++)body_change(mode,how,(ph_wire_format)f);
 }
 ph_status_cache_release(&bl_device);
 printf("S30 real HTTP/device: %u mutex waits + %u split-body changes; original OPEN/Basic/Bearer context, same-password restore/revoke/expiry/logout, receipts/GET, 4-slot busy, no-session login, MessagePack/CBOR PASS\n",serial,body_cases);
 return 0;
}

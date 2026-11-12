#define service_product_mp fixture_product_mp
#include "../service_mp/fixture.inc"
#undef service_product_mp
bool service_product_mp(const char *action,const ph_value *p,ph_writer *w,const char **out){
 if(!strcmp(action,"fresh_air.config.get")||!strcmp(action,"fresh_air.linkage.get")){
  assert(phv_keys(p,""));*out="ok";object(w);return true;
 }
 return fixture_product_mp(action,p,w,out);
}
#define main legacy_auth_main
#include "../local_auth/test_local_auth.c"
#undef main
#include "common/service_mp.h"
#include <time.h>
static unsigned validations,revoke_at;
bool __real_ph_auth_context_valid(const ph_auth_context *,unsigned);
bool __wrap_ph_auth_context_valid(const ph_auth_context *c,unsigned scope){
 if(++validations==revoke_at)ph_auth_local_revoke(c->owner,NULL);
 return __real_ph_auth_context_valid(c,scope);
}
static void lan(ph_buf *q,const ph_auth_context *ctx,const char *expected){
 ph_buf *out=service_lan_command_mp(q->buf,q->len,ctx);code(out,expected);
 ph_value root,id;assert(phv_parse(PH_WIRE_MSGPACK,out->buf,out->len,&root));
 if(phv_get(&root,"request_id",&id))assert(phv_equal_text(&id,"same"));
 ph_buf_free(out);ph_buf_free(q);
}
static void grant(ph_auth_state *s,const char *peer,unsigned scope,ph_auth_context *c){
 char key[33],token[33];assert(ph_auth_pair(s,"",peer,scope,key));assert(ph_auth_pair_resume_boot(s,peer,key,1,token));assert(ph_auth_udp_check(s,peer,token,c));
}
int main(void){
 service_init();fixture f;ph_auth_state s;ph_auth_context a,b,sensor;setup(&f,&s);
 grant(&s,"aabbccddeeff",PH_AUTH_SCOPE_CONTROL,&a);grant(&s,"112233445566",PH_AUTH_SCOPE_CONTROL,&b);grant(&s,"001122334455",PH_AUTH_SCOPE_SENSOR,&sensor);
 lan(request("fan.set","same",-1),&sensor,"auth_required");assert(!mutations);
 const char *denied[]={"config.get","config.set","wifi.scan","ota.start","system.reset","system.reboot","password.set","lan.pair","sensor.session.set","schedules.put","fresh_air.config.set","fresh_air.linkage.set",NULL};
 for(unsigned i=0;denied[i];i++)lan(request(denied[i],"same",-1),&a,"unsupported");assert(!mutations);
 lan(request("fan.set","same",-1),&a,"accepted");lan(request("fan.set","same",-1),&a,"accepted");assert(mutations==1);
 lan(request("light.set","same",-1),&a,"conflict");assert(mutations==1);
 lan(request("fan.set","same",-1),&b,"accepted");assert(mutations==2);
 command_code(request("fan.set","same",-1),"accepted");assert(mutations==3); /* HTTP receipt cannot alias */
 lan(request("status.get","same",-1),&a,"ok");lan(request("capabilities.get","same",-1),&a,"ok");lan(request("diagnostics.get","same",-1),&a,"ok");assert(captures==1&&caps==1&&diagnostics==1);
 lan(request("fresh_air.config.get","same",-1),&a,"ok");lan(request("fresh_air.linkage.get","same",-1),&a,"ok");assert(mutations==3);
 ph_buf *q=request("fan.set","next",-1);bool admitted=true;assert(service_control_begin());
 struct timespec before,after;clock_gettime(CLOCK_MONOTONIC,&before);
 ph_buf *out=service_lan_try_command_mp(q->buf,q->len,&a,&admitted);clock_gettime(CLOCK_MONOTONIC,&after);
 assert(!admitted);code(out,"busy");ph_buf_free(out);assert((after.tv_sec-before.tv_sec)*1000000000L+after.tv_nsec-before.tv_nsec<50000000L);
 service_control_end();out=service_lan_try_command_mp(q->buf,q->len,&a,&admitted);assert(admitted);code(out,"accepted");ph_buf_free(out);ph_buf_free(q);assert(mutations==4);
 /* The first check passes, but revoke just before the in-command check. */
 validations=0;revoke_at=2;lan(request("fan.set","same",-1),&a,"auth_required");assert(mutations==4);revoke_at=0;
 lan(request("status.get","same",-1),&a,"auth_required");
 test_heap_free=SERVICE_MEMORY_RESERVE;assert(!service_memory_try_claim(1024));
 puts("actual LAN dispatcher: CONTROL allowlist/peer+HTTP receipts/revoke/nonblocking/guard PASS");return 0;
}

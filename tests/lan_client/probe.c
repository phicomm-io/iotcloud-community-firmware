#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "common/local_auth.h"
#include "common/typed_wire.h"
#include "common/lan_client_core.inc"
static uint32_t clock_ms=100,epoch=1;static bool stored,fail_save;static char saved_key[33];static unsigned saved_scopes,sends;static uint8_t sequence=1;static unsigned char sent_wire[256];static size_t sent_length;
static uint32_t now(void){return clock_ms;}static uint32_t current_epoch(void){return epoch;}
static bool random_nonce(uint8_t out[16]){memset(out,sequence++,16);return true;}
static bool load(const char *peer,char key[33],unsigned *scope){assert(!strcmp(peer,"aabbccddeeff"));if(!stored)return false;strcpy(key,saved_key);*scope=saved_scopes;return true;}
static bool save(const char *peer,const char *key,unsigned scope,uint32_t e){assert(!strcmp(peer,"aabbccddeeff"));if(fail_save||e!=epoch)return false;strcpy(saved_key,key);saved_scopes=scope;stored=true;return true;}
static bool send_packet(const char *ip,const void *data,size_t n){assert(!strcmp(ip,"10.0.0.2"));assert(n<=sizeof(sent_wire));memcpy(sent_wire,data,n);sent_length=n;sends++;return true;}
static lc_ops ops={"001122334455",now,current_epoch,random_nonce,load,save,send_packet};
static void response(lc_state *s,const char *code,bool wrong_nonce,const char *ip){
 ph_value request,v;assert(phv_parse(PH_WIRE_MSGPACK,sent_wire,sent_length,&request));char op[20];assert(phv_get(&request,"op",&v)&&phv_text(&v,op,sizeof(op)));const void *nonce;size_t n;assert(phv_get(&request,"nonce",&v)&&phv_bytes(&v,&nonce,&n)&&n==16);
 unsigned char storage[256],secret[16],bad_nonce[16];memset(secret,0xab,16);memset(bad_nonce,0xee,16);ph_buf buffer;ph_writer w;assert(phw_init_fixed(&w,PH_WIRE_MSGPACK,&buffer,storage,sizeof(storage)));
 phw_begin(&w,NULL,false);phw_text(&w,"op",op);phw_text(&w,"src","aabbccddeeff");phw_text(&w,"code",code);phw_bytes_value(&w,"nonce",wrong_nonce?bad_nonce:nonce,16);phw_bytes_value(&w,!strcmp(op,"auth_pair")?"key":"token",secret,16);phw_end(&w);n=phw_finish_fixed(&w);assert(phv_parse(PH_WIRE_MSGPACK,storage,n,&v));assert(lc_response(s,&ops,&v,ip));memset(storage,0,sizeof(storage));
}
int main(void){lc_state state={0};const char *code;char password[33]="adminfjr";uint8_t token[16];
 assert(lc_start(&state,&ops,"aabbccddeeff","10.0.0.2",password,2,&code));assert(!strcmp(code,"accepted"));memset(password,0,sizeof(password));lc_tick(&state,&ops);assert(sends==1);
 ph_value root,p;char copied_password[33];assert(phv_parse(PH_WIRE_MSGPACK,sent_wire,sent_length,&root)&&phv_get(&root,"password",&p)&&phv_text(&p,copied_password,sizeof(copied_password))&&!strcmp(copied_password,"adminfjr"));
 response(&state,"ok",true,"10.0.0.2");assert(!stored);response(&state,"ok",false,"10.0.0.3");assert(!stored);
 response(&state,"ok",false,"10.0.0.2");assert(stored&&state.jobs[0].phase==LC_RESUME&&!state.jobs[0].password[0]);lc_tick(&state,&ops);response(&state,"ok",false,"10.0.0.2");assert(state.jobs[0].phase==LC_READY&&lc_token(&state,&ops,"aabbccddeeff","10.0.0.2",2,token));assert(token[0]==0xab);assert(!lc_token(&state,&ops,"aabbccddeeff","10.0.0.2",4,token));
 /* Device restart: persisted remote key alone establishes a new boot token. */
 memset(&state,0,sizeof(state));assert(!lc_token(&state,&ops,"aabbccddeeff","10.0.0.2",2,token));lc_tick(&state,&ops);assert(phv_parse(PH_WIRE_MSGPACK,sent_wire,sent_length,&root)&&phv_get(&root,"op",&p)&&phv_equal_text(&p,"auth_resume"));response(&state,"ok",false,"10.0.0.2");assert(state.jobs[0].phase==LC_READY);
 /* Changed local credentials invalidate in-flight work before persistence. */
 assert(lc_start(&state,&ops,"aabbccddeeff","10.0.0.2","adminfjr",2,&code));lc_tick(&state,&ops);epoch++;response(&state,"ok",false,"10.0.0.2");assert(state.jobs[0].phase==LC_FAILED&&!state.jobs[0].password[0]);
 /* Durable failure never yields a usable token; no RAM-only pairing success. */
 assert(lc_start(&state,&ops,"aabbccddeeff","10.0.0.2","adminfjr",2,&code));lc_tick(&state,&ops);fail_save=true;response(&state,"ok",false,"10.0.0.2");assert(state.jobs[0].phase==LC_FAILED&&!strcmp(state.jobs[0].code,"storage_error"));fail_save=false;
 assert(lc_start(&state,&ops,"aabbccddeeff","10.0.0.2","adminfjr",2,&code));lc_tick(&state,&ops);for(unsigned i=0;i<3;i++){clock_ms+=1000;lc_tick(&state,&ops);}assert(state.jobs[0].phase==LC_FAILED&&!strcmp(state.jobs[0].code,"timeout")&&!state.jobs[0].password[0]);
 puts("LAN pair sender identity/nonce/persistence/reboot/retry PASS");return 0;
}

#define AUTH_DEVICE_ID "D0BAE4636D3D"
#define main legacy_routes_main
#include "../auth_routes_mp/probe.c"
#undef main
#include "common/lan_client_core.inc"
static unsigned char packet_wire[256];static size_t packet_length;static uint32_t client_boot=7;static char saved[33];static unsigned saved_scope;static unsigned sequence;
static uint32_t client_now(void){return now_ms;}
static uint32_t client_epoch(void){return 1;}
static uint32_t business_boot(void){return client_boot;}
static bool client_random(uint8_t p[16]){memset(p,++sequence,16);return true;}
static bool client_load(const char *peer,char key[33],unsigned *scopes){assert(!strcmp(peer,"d0bae4636d3d"));if(!saved[0])return false;strcpy(key,saved);*scopes=saved_scope;return true;}
static bool client_save(const char *peer,const char *key,unsigned scopes,uint32_t epoch){assert(!strcmp(peer,"d0bae4636d3d")&&epoch==1);strcpy(saved,key);saved_scope=scopes;return true;}
static bool client_send(const char *ip,const void*p,size_t n){assert(!strcmp(ip,"192.0.2.2")&&n<=sizeof(packet_wire));memcpy(packet_wire,p,n);packet_length=n;return true;}
static lc_ops client_ops={"001122334455",client_now,client_epoch,client_random,client_load,client_save,client_send,business_boot};
static void exchange(lc_state*s){
 lc_tick(s,&client_ops);ph_value request,reply,v;assert(phv_parse(PH_WIRE_MSGPACK,packet_wire,packet_length,&request));
 if(phv_get(&request,"op",&v)&&phv_equal_text(&v,"auth_resume")){int64_t boot;assert(phv_get(&request,"boot",&v)&&phv_int(&v,&boot)&&boot==client_boot);}
 unsigned char response[256];ph_writer w;ph_buf b;assert(phw_init_fixed(&w,PH_WIRE_MSGPACK,&b,response,sizeof(response)));
 assert(feature_lan_pair(&request,&w,"192.0.2.1"));size_t n=phw_finish_fixed(&w);assert(n&&phv_parse(PH_WIRE_MSGPACK,response,n,&reply));
 assert(phv_get(&reply,"src",&v)&&phv_equal_text(&v,"d0bae4636d3d"));assert(lc_response(s,&client_ops,&reply,"192.0.2.2"));memset(response,0,sizeof(response));
}
int main(void){
 memset(&config,0,sizeof(config));memset(&config.store,255,sizeof(config.store));assert(device_local_auth_init());
 lc_state state={0};const char *code;uint8_t token[16],old[16];assert(lc_start(&state,&client_ops,"d0bae4636d3d","192.0.2.2","",PH_AUTH_SCOPE_CONTROL,&code));
 exchange(&state);assert(state.jobs[0].phase==LC_RESUME);exchange(&state);assert(state.jobs[0].phase==LC_READY);
 assert(lc_token(&state,&client_ops,"d0bae4636d3d","192.0.2.2",PH_AUTH_SCOPE_CONTROL,token));assert(feature_lan_auth("001122334455",token,PH_AUTH_SCOPE_CONTROL));memcpy(old,token,16);
 unsigned before=writes;memset(&state,0,sizeof(state));assert(!lc_token(&state,&client_ops,"d0bae4636d3d","192.0.2.2",1,token));exchange(&state);assert(lc_token(&state,&client_ops,"d0bae4636d3d","192.0.2.2",1,token)&&!memcmp(token,old,16)&&writes==before);
 client_boot++;memset(&state,0,sizeof(state));assert(!lc_token(&state,&client_ops,"d0bae4636d3d","192.0.2.2",1,token));exchange(&state);assert(lc_token(&state,&client_ops,"d0bae4636d3d","192.0.2.2",1,token)&&memcmp(token,old,16)&&writes==before);
 assert(!feature_lan_auth("001122334455",old,1)&&feature_lan_auth("001122334455",token,1));
 puts("actual uppercase TC1 auth adapter + lowercase sender pair/resume/boot PASS");return 0;
}

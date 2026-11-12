#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mico.h"
#include "../../common/typed_wire.h"
#include "../../common/local_auth.h"
#include "../../common/httpd/http.h"
typedef struct {char *body;size_t used;ph_auth_context auth;char scalar[1536];} ph_http_product;
static struct {char password[33];ph_auth_store store;} config,disk;
static mico_mutex_t config_mutex;
static bool recovery,fail_write;
static unsigned writes;
static uint32_t now_ms;
uint32_t mico_rtos_get_time(void){return now_ms;}
bool device_config_recovery(void){return recovery;}
void device_config_recovery_set(bool b){recovery=b;}
static int persist(void *context){assert(context==&config&&config_mutex);++writes;if(fail_write)return -1;disk=config;return 0;}
#define mico_system_context_update persist
#define AUTH_CONFIG_LOCK config_mutex
#define AUTH_STORE config.store
#define AUTH_PASSWORD config.password
#define AUTH_CONTEXT (&config)
#ifndef AUTH_DEVICE_ID
#define AUTH_DEVICE_ID "aabbccddeeff"
#endif
#include "../../common/local_auth_mico.inc"
#define HTTP_AUTH_MODEL "a1"
#define HTTP_AUTH_ID AUTH_DEVICE_ID
#define HTTP_AUTH_VERSION "host-fixture"
#define PHICOMM_FIRMWARE_VARIANT "full"
#include "../../common/httpd/local_auth.inc"
static ph_buf *request(const char *key,const char *text){ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,NULL,false);if(key)phw_text(&w,key,text);phw_end(&w);return phw_take(&w);}
static ph_value decode(ph_http_exchange *x){ph_value v;assert(!strcmp(x->response.type,"application/msgpack"));assert(phv_parse(PH_WIRE_MSGPACK,x->response.body,x->response.length,&v));return v;}
static void code(ph_http_exchange *x,unsigned status,const char *expected){ph_value v=decode(x),c;assert(x->response.status==status&&phv_get(&v,"code",&c)&&phv_equal_text(&c,expected));}
static void login(ph_http_product *p,ph_http_exchange *x,const char *pw,char token[33]){
    ph_buf *b=request("password",pw);p->body=b->buf;p->used=b->len;http_auth_complete(p,x);
    ph_value v=decode(x),data,t,required;if(phv_get(&v,"data",&data)){assert(phv_get(&data,"token",&t)&&phv_text(&t,token,33));assert(phv_get(&data,"password_required",&required)&&required.kind==(token[0]?PHV_TRUE:PHV_FALSE));}
    ph_buf_free(b);
}
static bool commit(void *p,const ph_auth_store *store){
    assert(local_auth_mutex);mico_rtos_lock_mutex(&config_mutex);
    __typeof__(config) old=config;strcpy(config.password,p);config.store=*store;
    bool ok=persist(&config)==0;if(!ok)config=old;mico_rtos_unlock_mutex(&config_mutex);return ok;
}
static ph_http_product auth_product;
static bool authorized(const char *header){memset(&auth_product,0,sizeof(auth_product));return http_auth_authorized(&auth_product,header);}
static void test_http(void){
    ph_http_product p={0};ph_http_request r={0};ph_http_exchange x={0};x.request=&r;
    strcpy(r.path,"/api/v1/auth/login");char token[33]={0},saved[33],a[40];
    assert(http_auth_route(r.path)&&!http_auth_route("/api/v1/config"));
    login(&p,&x,"wrong",token);code(&x,401,"unauthorized");
    for(unsigned i=0;i<12;i++){login(&p,&x,"",token);code(&x,200,"ok");assert(!token[0]);}
    for(unsigned i=0;i<PH_AUTH_HTTP_SLOTS;i++)assert(!local_auth.http[i].active);
    assert(authorized(NULL)&&authorized("")&&authorized("Basic YWRtaW46")&&authorized("Bearer obsolete"));
    http_auth_info(&p,&x);code(&x,200,"ok");ph_value root=decode(&x),data,v;
    assert(phv_get(&root,"data",&data)&&phv_get(&data,"password_set",&v)&&v.kind==PHV_FALSE);
    assert(!phv_get(&data,"ssid",&v)&&!phv_get(&data,"mqtt_password",&v));
    assert(phv_get(&data,"wire_formats",&v)&&v.kind==PHV_ARRAY);
    assert(ph_auth_credential_commit(device_local_auth(),"adminfjr",false,commit,"adminfjr"));
    assert(!http_auth_authorized(&auth_product,"Bearer obsolete")); /* original open context */
    assert(!authorized(NULL)&&!authorized("")&&!authorized("Basic YWRtaW46"));
    assert(authorized("Basic YWRtaW46YWRtaW5manI="));
    unsigned generation=local_auth.generation;
    assert(ph_auth_credential_commit(device_local_auth(),"adminfjr",true,commit,"adminfjr"));
    assert(!http_auth_authorized(&auth_product,"Basic YWRtaW46YWRtaW5manI=")); /* same-password restore */
    assert(authorized("Basic YWRtaW46YWRtaW5manI=")&&local_auth.generation==generation);
    login(&p,&x,"",token);code(&x,401,"unauthorized");
    login(&p,&x,"adminfjr",token);code(&x,200,"ok");strcpy(saved,token);
    snprintf(a,sizeof(a),"Bearer %s",saved);
    for(unsigned i=1;i<PH_AUTH_HTTP_SLOTS;i++){login(&p,&x,"adminfjr",token);code(&x,200,"ok");}
    login(&p,&x,"adminfjr",token);code(&x,503,"busy");
    login(&p,&x,"wrong",token);code(&x,401,"unauthorized");
    assert(authorized(a)&&authorized("Basic YWRtaW46YWRtaW5manI="));
    strcpy(r.path,"/api/v1/auth/logout");strcpy(r.authorization,a);
    ph_buf *empty=request(NULL,NULL);p.body=empty->buf;p.used=empty->len;
    http_auth_complete(&p,&x);code(&x,200,"ok");assert(!authorized(a));
    http_auth_complete(&p,&x);code(&x,401,"unauthorized");ph_buf_free(empty);
    strcpy(r.path,"/api/v1/auth/login");login(&p,&x,"adminfjr",token);code(&x,200,"ok");
    snprintf(a,sizeof(a),"Bearer %s",token);assert(authorized(a));
    now_ms+=PH_AUTH_IDLE_MS;assert(!http_auth_authorized(&auth_product,a)&&!authorized(a));
    const char json[]="{\"password\":\"adminfjr\"}";p.body=(char *)json;p.used=sizeof(json)-1;
    http_auth_complete(&p,&x);code(&x,400,"invalid_request");
}
static void test_lan(void){
    ph_writer request,reply;ph_value parsed,v;uint8_t nonce[16]={1};ph_buf b;unsigned char response[256];
    assert(phw_init(&request,PH_WIRE_MSGPACK));phw_begin(&request,NULL,false);
    phw_text(&request,"op","auth_pair");phw_text(&request,"src","001122334455");phw_text(&request,"password","adminfjr");phw_u32(&request,"scopes",2);phw_bytes_value(&request,"nonce",nonce,16);phw_end(&request);
    ph_buf *input=phw_take(&request);assert(phv_parse(PH_WIRE_MSGPACK,input->buf,input->len,&parsed));
    phw_init_fixed(&reply,PH_WIRE_MSGPACK,&b,response,sizeof(response));assert(feature_lan_pair(&parsed,&reply,"192.0.2.1"));size_t n=phw_finish_fixed(&reply);
    assert(phv_parse(PH_WIRE_MSGPACK,response,n,&parsed));assert(phv_get(&parsed,"code",&v)&&phv_equal_text(&v,"ok"));
    const void *raw;size_t length;assert(phv_get(&parsed,"key",&v)&&phv_bytes(&v,&raw,&length)&&length==16);
    char key[33],token[33];lan_auth_hex(raw,key);assert(ph_auth_pair_resume(device_local_auth(),"001122334455",key,token));
    uint8_t decoded[16];assert(ph_auth_token_decode(token,decoded));assert(feature_lan_auth("001122334455",decoded,2)&&!feature_lan_auth("001122334455",decoded,4));
    ph_buf_free(input);
    assert(phw_init(&request,PH_WIRE_MSGPACK));phw_begin(&request,NULL,false);
    phw_text(&request,"op","auth_unpair");phw_text(&request,"src","001122334455");
    phw_bytes_value(&request,"token",decoded,16);phw_bytes_value(&request,"nonce",nonce,16);phw_end(&request);
    input=phw_take(&request);
    unsigned before=writes;
    for(unsigned retry=0;retry<3;retry++){
        assert(phv_parse(PH_WIRE_MSGPACK,input->buf,input->len,&parsed));
        phw_init_fixed(&reply,PH_WIRE_MSGPACK,&b,response,sizeof(response));
        assert(feature_lan_pair(&parsed,&reply,"192.0.2.1"));n=phw_finish_fixed(&reply);
        assert(phv_parse(PH_WIRE_MSGPACK,response,n,&parsed));
        assert(phv_get(&parsed,"code",&v)&&phv_equal_text(&v,"ok"));
        assert(writes==before+1&&!feature_lan_auth("001122334455",decoded,2));
    }
    assert(phv_parse(PH_WIRE_MSGPACK,input->buf,input->len,&parsed));
    phw_init_fixed(&reply,PH_WIRE_MSGPACK,&b,response,sizeof(response));
    assert(feature_lan_pair(&parsed,&reply,"192.0.2.2"));n=phw_finish_fixed(&reply);
    assert(phv_parse(PH_WIRE_MSGPACK,response,n,&parsed));
    assert(phv_get(&parsed,"code",&v)&&phv_equal_text(&v,"auth_required"));
    ph_buf_free(input);
}
static void test_inbound_only_migration(void){
    char key[33],resumed[33];assert(ph_auth_pair(device_local_auth(),"adminfjr","001122334455",2,key));
    ph_auth_store expected=config.store;
    size_t prefix=offsetof(ph_auth_store,remote_count);
    memset((uint8_t*)&config.store+prefix,255,sizeof(config.store)-prefix);disk=config;
    unsigned before=writes;local_auth_ready=false;local_auth_mutex=0;recovery=false;
    assert(device_local_auth_init()&&!recovery&&writes==before+1);
    assert(!memcmp(&disk.store,&expected,sizeof(expected)));
    assert(ph_auth_pair_resume(device_local_auth(),"001122334455",key,resumed));
    // The next boot is already current and must not write again.
    before=writes;local_auth_ready=false;local_auth_mutex=0;
    assert(device_local_auth_init()&&writes==before&&!recovery);
    // A failed commit retains all persisted bytes and enters read-only recovery.
    memset((uint8_t*)&config.store+prefix,255,sizeof(config.store)-prefix);disk=config;
    ph_auth_store durable=disk.store;fail_write=true;before=writes;
    local_auth_ready=false;local_auth_mutex=0;
    assert(device_local_auth_init()&&recovery&&writes==before+1);
    assert(!memcmp(&disk.store,&durable,sizeof(durable)));
    assert(!ph_auth_pair(device_local_auth(),"adminfjr","aabbccddeeff",1,resumed));
    assert(writes==before+1);fail_write=false;recovery=false;
}
static void test_unknown_trailer_recovery(void){
    /* Simulate a fresh boot over a future schema. Preserve both the password
     * and opaque trailer; the recovery gate must prevent a pairing write. */
    config.store.version=PH_AUTH_STORE_VERSION+1;
    ph_auth_store before=config.store;unsigned before_writes=writes;
    local_auth_ready=false;local_auth_mutex=0;recovery=false;
    assert(device_local_auth_init()&&recovery&&writes==before_writes);
    assert(!memcmp(&config.store,&before,sizeof(before)));
    assert(ph_auth_password_is_set(device_local_auth()));
    char key[33];assert(!ph_auth_pair(device_local_auth(),"adminfjr","001122334455",2,key));
    assert(writes==before_writes&&!memcmp(&config.store,&before,sizeof(before)));
}
int main(void){
    memset(&config,0,sizeof(config));memset(&config.store,255,sizeof(config.store));
    assert(device_local_auth_init()&&writes==1&&ph_auth_store_valid(&disk.store)&&!disk.password[0]);
    test_http();test_lan();test_inbound_only_migration();test_unknown_trailer_recovery();assert(!local_auth_mutex&&!config_mutex);
    puts("MP auth: open HTTP without sessions, admin Basic, 401/503, logout/expiry/change/restore, public info and UDP scopes PASS");
    return 0;
}

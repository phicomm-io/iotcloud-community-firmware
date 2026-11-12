#include "../../common/local_auth.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
typedef struct {uint32_t now,sequence;char password[33];bool fail_random,fail_save;unsigned saves;ph_auth_store disk;pthread_mutex_t mutex;} fixture;
static uint32_t now(void*u){return ((fixture*)u)->now;}
static bool rng(void*u,uint8_t*p,size_t n){fixture*f=u;size_t i;if(f->fail_random)return false;memset(p,0,n);uint32_t v=++f->sequence;for(i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));return true;}
static bool password(void*u,char*p){memcpy(p,((fixture*)u)->password,33);return true;}
static bool save(void*u,const ph_auth_store*p){fixture*f=u;if(f->fail_save)return false;f->disk=*p;f->saves++;return true;}
static void lock(void*u){assert(!pthread_mutex_lock(&((fixture*)u)->mutex));}
static void unlock(void*u){assert(!pthread_mutex_unlock(&((fixture*)u)->mutex));}
static void setup(fixture*f,ph_auth_state*s){memset(f,0,sizeof(*f));assert(!pthread_mutex_init(&f->mutex,0));ph_auth_store_empty(&f->disk);ph_auth_ops o={f,now,rng,lock,unlock,password,save};assert(ph_auth_init(s,&o,&f->disk));}
static void bearer(char out[40],const char*t){strcpy(out,"Bearer ");strcat(out,t);}
static void test_unset_http(void){
    fixture f;ph_auth_state s;ph_auth_context c;char token[33]="old token";
    setup(&f,&s);f.fail_random=true;
    assert(ph_auth_http_login_result(&s,"wrong",token)==PH_AUTH_LOGIN_DENIED&&!token[0]);
    const char *headers[]={NULL,"","Basic invalid","Bearer obsolete"};
    for(unsigned i=0;i<20;i++){
        strcpy(token,"old token");
        assert(ph_auth_http_login_result(&s,"",token)==PH_AUTH_LOGIN_OK&&!token[0]);
        assert(ph_auth_http_check(&s,headers[i%4],&c)&&c.source==PH_AUTH_HTTP_OPEN);
        assert(ph_auth_context_valid(&c,PH_AUTH_SCOPE_ALL));
    }
    assert(!s.generation&&!f.sequence&&!f.saves);
    for(unsigned i=0;i<PH_AUTH_HTTP_SLOTS;i++)assert(!s.http[i].active);
    /* HTTP's open policy never grants UDP or MQTT sessions. */
    assert(!ph_auth_udp_check(&s,"controller","",&c));
    assert(!ph_auth_udp_check(&s,"controller","1234567890abcdef1234567890abcdef",&c));
    assert(!ph_auth_pair(&s,"wrong","controller",PH_AUTH_SCOPE_SENSOR,token));
    assert(!ph_auth_mqtt_grant(&s,"cloud","",PH_AUTH_SCOPE_SENSOR));
}
static void test_basic(void){
    fixture f;ph_auth_state s;ph_auth_context c;char t[33],a[40],key[33],udp[33];
    setup(&f,&s);strcpy(f.password,"pw");
    assert(!ph_auth_http_check(&s,NULL,&c)&&!ph_auth_http_check(&s,"",&c));
    assert(!ph_auth_http_check(&s,"Basic YWRtaW46",&c));
    assert(!ph_auth_http_check(&s,"Basic dXNlcjpwdw==",&c));
    assert(!ph_auth_http_check(&s,"basic YWRtaW46cHc=",&c));
    assert(!ph_auth_http_check(&s,"Basic YWRtaW46cHc=extra",&c));
    f.fail_random=true;
    assert(ph_auth_http_check(&s,"Basic YWRtaW46cHc=",&c)&&c.source==PH_AUTH_HTTP_BASIC);
    assert(ph_auth_context_valid(&c,PH_AUTH_SCOPE_ALL)&&!s.generation&&!f.sequence);
    f.fail_random=false;
    assert(!ph_auth_http_login(&s,"wrong",t));assert(ph_auth_http_login(&s,"pw",t));
    bearer(a,t);assert(ph_auth_http_check(&s,a,&c)&&c.source==PH_AUTH_HTTP);
    assert(ph_auth_context_valid(&c,PH_AUTH_SCOPE_ADMIN));assert(!ph_auth_http_check(&s,t,&c));
    assert(ph_auth_pair(&s,"pw","controller",PH_AUTH_SCOPE_SENSOR,key));
    assert(ph_auth_pair_resume(&s,"controller",key,udp));
    assert(!ph_auth_udp_check(&s,"controller",t,&c));
    bearer(a,udp);assert(!ph_auth_http_check(&s,a,&c));
    assert(!ph_auth_pair_resume(&s,"other",key,udp));
    assert(ph_auth_udp_check(&s,"controller",udp,&c));
    assert(ph_auth_context_valid(&c,PH_AUTH_SCOPE_SENSOR));
    assert(!ph_auth_context_valid(&c,PH_AUTH_SCOPE_ADMIN));
    assert(!ph_auth_pair(&s,"pw","admin",PH_AUTH_SCOPE_ADMIN,key));
}
static void test_expiry(void){
    fixture f;ph_auth_state s;ph_auth_context c;char t[33],a[40];setup(&f,&s);
    strcpy(f.password,"pw");f.now=UINT_MAX-100;
    assert(ph_auth_http_login(&s,"pw",t));bearer(a,t);f.now+=1000;
    assert(ph_auth_http_check(&s,a,&c));f.now+=PH_AUTH_IDLE_MS;
    assert(!ph_auth_context_valid(&c,PH_AUTH_SCOPE_CONTROL));assert(!ph_auth_http_check(&s,a,&c));
    assert(ph_auth_http_login(&s,"pw",t));bearer(a,t);
    for(unsigned i=0;i<48;i++){f.now+=PH_AUTH_IDLE_MS-1;assert(ph_auth_http_check(&s,a,&c));}
    f.now+=48;assert(!ph_auth_http_check(&s,a,&c));
}
static void test_capacity_failure(void){
    fixture f;ph_auth_state s;ph_auth_context c,old;char t[33],saved[33],a[40],key[33];
    setup(&f,&s);strcpy(f.password,"pw");
    for(unsigned i=0;i<PH_AUTH_HTTP_SLOTS;i++)assert(ph_auth_http_login(&s,"pw",t));
    strcpy(saved,t);bearer(a,saved);assert(ph_auth_http_check(&s,a,&old));
    assert(ph_auth_http_login_result(&s,"pw",t)==PH_AUTH_LOGIN_BUSY&&!t[0]);
    assert(ph_auth_http_login_result(&s,"wrong",t)==PH_AUTH_LOGIN_DENIED&&!t[0]);
    assert(ph_auth_context_valid(&old,PH_AUTH_SCOPE_ALL));
    assert(ph_auth_http_check(&s,"Basic YWRtaW46cHc=",&c));
    assert(ph_auth_http_logout(&s,a)&&!ph_auth_http_logout(&s,a));
    assert(!ph_auth_context_valid(&old,PH_AUTH_SCOPE_ALL));
    assert(ph_auth_http_login(&s,"pw",t));
    assert(!ph_auth_context_valid(&old,PH_AUTH_SCOPE_ALL)); /* reused slot */
    f.now+=PH_AUTH_IDLE_MS;f.fail_random=true;
    assert(ph_auth_http_login_result(&s,"pw",t)==PH_AUTH_LOGIN_BUSY&&!t[0]);
    assert(ph_auth_http_check(&s,"Basic YWRtaW46cHc=",&c));
    assert(!ph_auth_pair(&s,"pw","peer",1,key));assert(s.store.count==0);
    f.fail_random=false;f.fail_save=true;
    assert(!ph_auth_pair(&s,"pw","peer",1,key));assert(s.store.count==0);
    f.fail_save=false;
    for(unsigned i=0;i<PH_AUTH_PAIR_SLOTS;i++){char p[13];snprintf(p,sizeof(p),"peer%u",i);assert(ph_auth_pair(&s,"pw",p,1,key));}
    assert(!ph_auth_pair(&s,"pw","overflow",1,key));assert(s.store.count==PH_AUTH_PAIR_SLOTS);
    assert(ph_auth_pair_remove(&s,"peer0"));assert(s.store.count==PH_AUTH_PAIR_SLOTS-1);
    assert(ph_auth_pair(&s,"pw","overflow",1,key));
}
static void test_boot_revoke_mqtt(void){
    fixture f;ph_auth_state s,reboot;ph_auth_context c,old;char key[33],udp[33],http[33],a[40];
    setup(&f,&s);strcpy(f.password,"pw");
    assert(ph_auth_http_login(&s,"pw",http));bearer(a,http);assert(ph_auth_http_check(&s,a,&old));
    assert(ph_auth_pair(&s,"pw","sensor",2,key));assert(ph_auth_pair_resume(&s,"sensor",key,udp));
    assert(ph_auth_init(&reboot,&s.ops,&f.disk));assert(!ph_auth_http_check(&reboot,a,&c));
    assert(!ph_auth_udp_check(&reboot,"sensor",udp,&c));assert(ph_auth_pair_resume(&reboot,"sensor",key,udp));
    const char*mt="fedcba9876543210fedcba9876543210";assert(ph_auth_mqtt_grant(&s,"mqttsensor",mt,2));
    assert(ph_auth_password_changed(&s));assert(!ph_auth_context_valid(&old,1));
    assert(!ph_auth_pair_resume(&s,"sensor",key,udp));assert(ph_auth_udp_check(&s,"mqttsensor",mt,&c));
    assert(ph_auth_context_valid(&c,2));ph_auth_mqtt_context(&s,&c);assert(ph_auth_context_valid(&c,8));
    strcpy(f.password,"changed");assert(!ph_auth_http_login(&s,"",http));
    assert(ph_auth_http_login(&s,"changed",http));assert(ph_auth_udp_check(&s,"mqttsensor",mt,&c));
}
static void test_pending_pair_epoch(void){fixture f;ph_auth_state s;char t[33];setup(&f,&s);uint32_t before=ph_auth_credential_epoch(&s);assert(before);assert(ph_auth_http_login(&s,"",t));assert(ph_auth_credential_epoch(&s)==before);const char *key="1234567890abcdef1234567890abcdef";assert(ph_auth_pair_store_epoch(&s,"peer",key,2,before));assert(ph_auth_password_changed(&s));unsigned saves=f.saves;assert(ph_auth_credential_epoch(&s)!=before);assert(!ph_auth_pair_store_epoch(&s,"peer",key,2,before));assert(!s.store.count&&f.saves==saves);uint32_t next=ph_auth_credential_epoch(&s);assert(ph_auth_pair_store_epoch(&s,"peer",key,2,next));ph_auth_local_revoke(&s,NULL);assert(ph_auth_credential_epoch(&s)!=next);assert(!ph_auth_pair_store_epoch(&s,"peer",key,2,next));}
typedef struct{ph_auth_state*s;const char *password;unsigned success;} thread;
static void test_pair_retry(void){
    fixture f;ph_auth_state s;ph_auth_context ctx;char key[33],retry[33],token[33],again[33];
    setup(&f,&s);f.now=UINT_MAX-100;
    assert(ph_auth_pair(&s,"","peer",PH_AUTH_SCOPE_SENSOR,key));
    unsigned saves=f.saves;uint32_t epoch=ph_auth_credential_epoch(&s);
    f.fail_random=f.fail_save=true;
    assert(!ph_auth_pair(&s,"wrong","peer",PH_AUTH_SCOPE_SENSOR,retry));
    assert(ph_auth_pair(&s,"","peer",PH_AUTH_SCOPE_SENSOR,retry));
    assert(!strcmp(key,retry)&&f.saves==saves&&ph_auth_credential_epoch(&s)==epoch);
    f.fail_random=f.fail_save=false;
    assert(ph_auth_pair_resume(&s,"peer",key,token));
    assert(ph_auth_udp_check(&s,"peer",token,&ctx));
    uint32_t generation=ctx.generation;f.now+=1000;
    f.fail_random=true;
    assert(ph_auth_pair_resume(&s,"peer",key,again)&&!strcmp(token,again));
    assert(ph_auth_context_valid(&ctx,PH_AUTH_SCOPE_SENSOR)&&ctx.generation==generation);
    assert(!ph_auth_pair_resume(&s,"other",key,again));
    assert(!ph_auth_pair_resume(&s,"peer",token,again));
    /* Duplicate incoming credentials must not revoke an existing session either. */
    assert(ph_auth_pair_store_epoch(&s,"peer",key,PH_AUTH_SCOPE_SENSOR,epoch));
    assert(f.saves==saves&&ph_auth_context_valid(&ctx,PH_AUTH_SCOPE_SENSOR));
    f.now+=PH_AUTH_IDLE_MS-1000;
    assert(!ph_auth_pair_resume(&s,"peer",key,again)); /* expired + RNG failure */
    assert(!ph_auth_context_valid(&ctx,PH_AUTH_SCOPE_SENSOR));
    f.fail_random=false;
    assert(ph_auth_pair_resume(&s,"peer",key,again)&&strcmp(token,again));
    strcpy(token,again);
    /* Scope changes rotate the key and revoke old contexts. */
    assert(ph_auth_udp_check(&s,"peer",token,&ctx));
    assert(ph_auth_pair(&s,"","peer",PH_AUTH_SCOPE_GROUP,retry)&&strcmp(key,retry));
    assert(!ph_auth_context_valid(&ctx,PH_AUTH_SCOPE_SENSOR));
    assert(!ph_auth_pair_resume(&s,"peer",key,again));
    assert(ph_auth_pair_resume(&s,"peer",retry,token));
    uint32_t born=s.local[0].born;
    for(unsigned i=0;i<48;i++){
        f.now+=PH_AUTH_IDLE_MS-1;
        assert(ph_auth_pair_resume(&s,"peer",retry,again)&&!strcmp(token,again));
        assert(ph_auth_udp_check(&s,"peer",token,&ctx));
        assert(s.local[0].born==born);
    }
    f.now+=48;
    assert(ph_auth_pair_resume(&s,"peer",retry,again)&&strcmp(token,again));
    assert(!ph_auth_context_valid(&ctx,PH_AUTH_SCOPE_GROUP));
    assert(ph_auth_pair_remove(&s,"peer"));
    assert(!ph_auth_pair_resume(&s,"peer",retry,again));
    assert(ph_auth_pair(&s,"","peer",PH_AUTH_SCOPE_GROUP,key)&&strcmp(key,retry));
    strcpy(f.password,"changed");
    assert(!ph_auth_pair(&s,"","peer",PH_AUTH_SCOPE_GROUP,again));
    assert(!ph_auth_pair_resume(&s,"peer",key,again));
    assert(ph_auth_pair(&s,"changed","peer",PH_AUTH_SCOPE_GROUP,again)&&strcmp(key,again));
}
static void*concurrent(void*u){thread*t=u;char token[33];if(ph_auth_http_login(t->s,t->password,token))t->success=1;return 0;}
static void test_concurrent(void){
    for(unsigned configured=0;configured<2;configured++){
        fixture f;ph_auth_state s;pthread_t ids[12];thread t[12];unsigned n=0;
        setup(&f,&s);strcpy(f.password,configured?"pw":"");
        for(unsigned i=0;i<12;i++){t[i]=(thread){&s,f.password,0};assert(!pthread_create(&ids[i],0,concurrent,&t[i]));}
        for(unsigned i=0;i<12;i++){pthread_join(ids[i],0);n+=t[i].success;}
        assert(n==(configured?PH_AUTH_HTTP_SLOTS:12));
    }
}
static void test_failed_password_transaction(void){
 fixture f;ph_auth_state s,reboot;char key[33],token[33];setup(&f,&s);
 assert(ph_auth_pair(&s,"","peer",2,key));assert(ph_auth_pair_resume(&s,"peer",key,token));
 f.fail_save=true;assert(!ph_auth_password_changed(&s));
 assert(!ph_auth_pair_resume(&s,"peer",key,token));
 /* Failed revoke is NOT a committed password change; the old credential
  * and old pair remain durable and return together on a reboot. */
 assert(!f.password[0]);assert(ph_auth_init(&reboot,&s.ops,&f.disk));
 assert(ph_auth_pair_resume(&reboot,"peer",key,token));
 f.fail_save=false;assert(ph_auth_password_changed(&reboot));
 /* Crash before password commit: pairs must still be gone on boot. */
 assert(ph_auth_init(&s,&reboot.ops,&f.disk));
 assert(!ph_auth_pair_resume(&s,"peer",key,token));
 strcpy(f.password,"new-password");
 assert(ph_auth_init(&reboot,&s.ops,&f.disk));
 assert(!ph_auth_pair_resume(&reboot,"peer",key,token));
 assert(!ph_auth_http_login(&reboot,"",token));
 assert(ph_auth_http_login(&reboot,"new-password",token));
}
typedef struct {fixture *f;const char *password;bool fail;} commit_fixture;
static bool commit_password(void *u,const ph_auth_store *store){
    commit_fixture *c=u;
    /* Auth lock must cover both pairing trailer and credential write. */
    assert(pthread_mutex_trylock(&c->f->mutex)!=0);
    if(c->fail)return false;
    c->f->disk=*store;strcpy(c->f->password,c->password);++c->f->saves;
    return true;
}
static void test_stateless_http_epoch(void){
    fixture f;ph_auth_state s;ph_auth_context open,basic;setup(&f,&s);
    assert(ph_auth_http_check(&s,"",&open));
    commit_fixture c={&f,"",false};uint32_t epoch=ph_auth_credential_epoch(&s);
    assert(ph_auth_credential_commit(&s,"",false,commit_password,&c));
    assert(ph_auth_context_valid(&open,PH_AUTH_SCOPE_ALL)&&ph_auth_credential_epoch(&s)==epoch);
    c.fail=true;assert(!ph_auth_credential_commit(&s,"",true,commit_password,&c));
    assert(ph_auth_context_valid(&open,PH_AUTH_SCOPE_ALL));
    c.fail=false;assert(ph_auth_credential_commit(&s,"",true,commit_password,&c));
    assert(!ph_auth_context_valid(&open,PH_AUTH_SCOPE_CONTROL));
    assert(ph_auth_http_check(&s,NULL,&open));
    c.password="pw";assert(ph_auth_credential_commit(&s,"pw",false,commit_password,&c));
    assert(!ph_auth_context_valid(&open,PH_AUTH_SCOPE_ALL));
    assert(ph_auth_http_check(&s,"Basic YWRtaW46cHc=",&basic));
    c.fail=true;assert(!ph_auth_credential_commit(&s,"pw",true,commit_password,&c));
    assert(ph_auth_context_valid(&basic,PH_AUTH_SCOPE_ALL));
    c.fail=false;assert(ph_auth_credential_commit(&s,"pw",false,commit_password,&c));
    assert(ph_auth_context_valid(&basic,PH_AUTH_SCOPE_ALL));
    assert(ph_auth_credential_commit(&s,"pw",true,commit_password,&c));
    assert(!ph_auth_context_valid(&basic,PH_AUTH_SCOPE_ALL));
    assert(ph_auth_http_check(&s,"Basic YWRtaW46cHc=",&basic));
    c.password="";assert(ph_auth_credential_commit(&s,"",false,commit_password,&c));
    assert(!ph_auth_context_valid(&basic,PH_AUTH_SCOPE_ALL)&&!ph_auth_context_valid(&open,PH_AUTH_SCOPE_ALL));
    assert(ph_auth_http_check(&s,"",&open));ph_auth_local_revoke(&s,NULL);
    assert(!ph_auth_context_valid(&open,PH_AUTH_SCOPE_ALL));
    assert(ph_auth_http_check(&s,"",&open));f.fail_save=true;
    assert(!ph_auth_password_changed(&s)&&!ph_auth_context_valid(&open,PH_AUTH_SCOPE_ALL));
    assert(ph_auth_http_check(&s,"",&open));strcpy(f.password,"pw");
    assert(!ph_auth_context_valid(&open,PH_AUTH_SCOPE_ALL));
    assert(ph_auth_http_check(&s,"Basic YWRtaW46cHc=",&basic));strcpy(f.password,"");
    assert(!ph_auth_context_valid(&basic,PH_AUTH_SCOPE_ALL));
}
static void test_atomic_credential_commit(void){
    fixture f;ph_auth_state s,reboot;ph_auth_context ctx;char key[33],udp[33],http[33],a[40];
    setup(&f,&s);assert(ph_auth_pair(&s,"","peer",2,key));
    assert(ph_auth_pair_resume(&s,"peer",key,udp));assert(ph_auth_http_login(&s,"",http));
    bearer(a,http);assert(ph_auth_http_check(&s,a,&ctx));
    const char *mqtt="fedcba9876543210fedcba9876543210";
    assert(ph_auth_mqtt_grant(&s,"cloud",mqtt,2));
    commit_fixture c={&f,"adminfjr",true};uint32_t epoch=ph_auth_credential_epoch(&s);
    assert(!ph_auth_credential_commit(&s,c.password,false,commit_password,&c));
    assert(!f.password[0]&&f.disk.count==1&&ph_auth_credential_epoch(&s)==epoch);
    assert(ph_auth_context_valid(&ctx,1));assert(ph_auth_pair_resume(&s,"peer",key,udp));
    c.fail=false;assert(ph_auth_credential_commit(&s,c.password,false,commit_password,&c));
    assert(!f.disk.count&&!ph_auth_context_valid(&ctx,1));
    assert(!ph_auth_pair_resume(&s,"peer",key,udp));
    assert(ph_auth_udp_check(&s,"cloud",mqtt,&ctx));
    assert(ph_auth_init(&reboot,&s.ops,&f.disk));
    assert(!ph_auth_pair_resume(&reboot,"peer",key,udp));
    assert(ph_auth_http_login_result(&reboot,"",http)==PH_AUTH_LOGIN_DENIED);
    assert(ph_auth_http_login_result(&reboot,"adminfjr",http)==PH_AUTH_LOGIN_OK);
    bearer(a,http);assert(ph_auth_http_check(&reboot,a,&ctx));
    assert(ph_auth_pair(&reboot,"adminfjr","peer",2,key));
    epoch=ph_auth_credential_epoch(&reboot);
    assert(ph_auth_credential_commit(&reboot,c.password,false,commit_password,&c));
    assert(ph_auth_credential_epoch(&reboot)==epoch&&f.disk.count==1&&ph_auth_context_valid(&ctx,1));
    assert(ph_auth_credential_commit(&reboot,c.password,true,commit_password,&c));
    assert(ph_auth_credential_epoch(&reboot)!=epoch&&!f.disk.count&&!ph_auth_context_valid(&ctx,1));
    assert(ph_auth_password_is_set(&reboot));
    for(unsigned i=0;i<4;i++)assert(ph_auth_http_login(&reboot,"adminfjr",http));
    assert(ph_auth_http_login_result(&reboot,"adminfjr",http)==PH_AUTH_LOGIN_BUSY);
    assert(ph_auth_http_login_result(&reboot,"wrong",http)==PH_AUTH_LOGIN_DENIED);
}
static void test_remote_direction_and_revocation(void){
    fixture f;ph_auth_state s,reboot;char key[33],out[33],token[33];unsigned scopes;ph_auth_context ctx;
    setup(&f,&s);const char *remote="1234567890abcdef1234567890abcdef";
    uint32_t epoch=ph_auth_credential_epoch(&s);
    assert(!ph_auth_remote_store(&s,"target",remote,2,0));
    assert(ph_auth_remote_store(&s,"target",remote,2,epoch));
    assert(ph_auth_remote_get(&s,"target",out,&scopes)&&!strcmp(out,remote)&&scopes==2);
    unsigned saves=f.saves;
    assert(ph_auth_remote_store(&s,"target",remote,2,epoch)&&f.saves==saves);
    assert(!ph_auth_pair_resume(&s,"target",remote,token));
    assert(!ph_auth_udp_check(&s,"target",remote,&ctx));
    assert(ph_auth_pair(&s,"","target",2,key)&&strcmp(key,remote));
    assert(ph_auth_init(&reboot,&s.ops,&f.disk));
    assert(ph_auth_remote_get(&reboot,"target",out,&scopes));
    f.fail_save=true;assert(!ph_auth_remote_remove(&reboot,"target"));
    assert(ph_auth_remote_get(&reboot,"target",out,&scopes));
    f.fail_save=false;assert(ph_auth_remote_remove(&reboot,"target"));
    assert(!ph_auth_remote_get(&reboot,"target",out,&scopes));
    assert(ph_auth_remote_store(&reboot,"target",remote,2,epoch));
    commit_fixture c={&f,"adminfjr",false};
    assert(ph_auth_credential_commit(&reboot,c.password,false,commit_password,&c));
    assert(!f.disk.remote_count&&!f.disk.count);
    assert(!ph_auth_remote_store(&reboot,"target",remote,2,epoch));
    assert(ph_auth_init(&s,&reboot.ops,&f.disk));
    assert(!ph_auth_remote_get(&s,"target",out,&scopes));
    ph_auth_store invalid=f.disk;invalid.remote_count=PH_AUTH_PAIR_SLOTS+1;
    assert(!ph_auth_store_valid(&invalid));invalid=f.disk;invalid.reserved=1;assert(!ph_auth_store_valid(&invalid));
}
static void test_pair_compaction_revokes_sessions(void){
 fixture f;ph_auth_state s;ph_auth_context context,other;char key[3][33],token[3][33],renewed[33];
 const char *peers[]={"peer-a","peer-b","peer-c"};setup(&f,&s);
 for(unsigned i=0;i<3;i++){assert(ph_auth_pair(&s,"",peers[i],1,key[i]));assert(ph_auth_pair_resume(&s,peers[i],key[i],token[i]));}
 const char *mqtt="fedcba9876543210fedcba9876543210";
 assert(ph_auth_mqtt_grant(&s,"cloud",mqtt,2));
 assert(ph_auth_udp_check(&s,peers[1],token[1],&other));
 f.fail_save=true;assert(!ph_auth_pair_unpair(&s,peers[0],token[0]));
 assert(ph_auth_udp_check(&s,peers[2],token[2],&context));
 f.fail_save=false;assert(ph_auth_pair_unpair(&s,peers[0],token[0]));
 /* Compacting durable records preserves unrelated slots and contexts. */
 assert(ph_auth_context_valid(&other,1));
 assert(ph_auth_udp_check(&s,peers[1],token[1],&context));
 assert(ph_auth_udp_check(&s,peers[2],token[2],&context));
 assert(ph_auth_pair_resume(&s,peers[2],key[2],renewed)&&!strcmp(renewed,token[2]));
 assert(ph_auth_pair_unpair(&s,peers[2],renewed));
 assert(!ph_auth_udp_check(&s,peers[2],renewed,&context));
 assert(!ph_auth_udp_check(&s,peers[2],token[2],&context));
 assert(ph_auth_udp_check(&s,"cloud",mqtt,&context));
 assert(ph_auth_context_valid(&other,1));
 assert(ph_auth_udp_check(&s,peers[1],token[1],&context));
 assert(ph_auth_pair_resume(&s,peers[1],key[1],renewed));
 assert(ph_auth_pair(&s,"",peers[1],2,key[1]));
 assert(!ph_auth_udp_check(&s,peers[1],renewed,&context));
 assert(ph_auth_pair_resume(&s,peers[1],key[1],renewed));
 assert(ph_auth_udp_check(&s,peers[1],renewed,&context)&&ph_auth_context_valid(&context,2));
 assert(!ph_auth_context_valid(&context,1));
 assert(ph_auth_pair(&s,"",peers[0],1,key[0]));
 assert(ph_auth_pair_resume(&s,peers[0],key[0],token[0]));
 assert(ph_auth_pair_remove(&s,peers[1]));
 assert(!ph_auth_udp_check(&s,peers[1],renewed,&context));
 assert(ph_auth_udp_check(&s,peers[0],token[0],&context));
}
static void test_unpair_receipts(void){
 fixture f;ph_auth_state s,reboot;ph_auth_context other,c;char key[33],token[33],bkey[33],btoken[33];
 uint8_t nonce[16]={1},wrong[16]={2};setup(&f,&s);f.now=UINT_MAX-100;
 assert(ph_auth_pair(&s,"","peer-b",1,bkey)&&ph_auth_pair_resume(&s,"peer-b",bkey,btoken));
 assert(ph_auth_udp_check(&s,"peer-b",btoken,&other));
 const char *mqtt="fedcba9876543210fedcba9876543210";assert(ph_auth_mqtt_grant(&s,"cloud",mqtt,2));
 assert(ph_auth_pair(&s,"","peer",1,key)&&ph_auth_pair_resume(&s,"peer",key,token));
 f.fail_save=true;unsigned saves=f.saves;
 assert(!ph_auth_pair_unpair_request(&s,"peer",token,"192.0.2.1",nonce));
 assert(!ph_auth_pair_unpair_request(&s,"peer",token,"192.0.2.1",nonce));
 assert(f.saves==saves&&ph_auth_udp_check(&s,"peer",token,&c));
 f.fail_save=false;assert(ph_auth_pair_unpair_request(&s,"peer",token,"192.0.2.1",nonce));
 assert(f.saves==saves+1&&!ph_auth_udp_check(&s,"peer",token,&c));
 f.now+=1000;assert(ph_auth_pair_unpair_request(&s,"peer",token,"192.0.2.1",nonce));
 assert(f.saves==saves+1); /* Lost ACK retries never write or revive credentials. */
 assert(!ph_auth_pair_unpair_request(&s,"other",token,"192.0.2.1",nonce));
 assert(!ph_auth_pair_unpair_request(&s,"peer",token,"192.0.2.2",nonce));
 assert(!ph_auth_pair_unpair_request(&s,"peer",token,"192.0.2.1",wrong));
 assert(!ph_auth_pair_unpair_request(&s,"peer",mqtt,"192.0.2.1",nonce));
 assert(!ph_auth_pair_unpair_request(&s,"peer",token,NULL,nonce));
 assert(!ph_auth_pair_unpair_request(&s,"peer",token,"1234567890123456",nonce));
 assert(ph_auth_context_valid(&other,1)&&ph_auth_udp_check(&s,"cloud",mqtt,&c));
 assert(ph_auth_init(&reboot,&s.ops,&f.disk));
 assert(!ph_auth_pair_unpair_request(&reboot,"peer",token,"192.0.2.1",nonce));
 f.now+=PH_AUTH_UNPAIR_RECEIPT_MS-1000;
 assert(!ph_auth_pair_unpair_request(&s,"peer",token,"192.0.2.1",nonce)); /* wrap + exact expiry */
 assert(ph_auth_pair(&s,"","peer",1,key)&&ph_auth_pair_resume(&s,"peer",key,token));
 assert(ph_auth_pair_unpair_request(&s,"peer",token,"192.0.2.1",nonce));
 ph_auth_local_revoke(&s,NULL);assert(!ph_auth_pair_unpair_request(&s,"peer",token,"192.0.2.1",nonce));
 assert(ph_auth_pair(&s,"","peer",1,key)&&ph_auth_pair_resume(&s,"peer",key,token));
 assert(ph_auth_pair_unpair_request(&s,"peer",token,"192.0.2.1",nonce));
 commit_fixture commit={&f,"adminfjr",false};
 assert(ph_auth_credential_commit(&s,commit.password,false,commit_password,&commit));
 assert(!ph_auth_pair_unpair_request(&s,"peer",token,"192.0.2.1",nonce));
 assert(ph_auth_udp_check(&s,"cloud",mqtt,&c));
 assert(ph_auth_pair(&s,"adminfjr","peer",1,key)&&ph_auth_pair_resume(&s,"peer",key,token));
 assert(ph_auth_pair_unpair_request(&s,"peer",token,"192.0.2.1",nonce));
 commit.fail=true;
 assert(!ph_auth_credential_commit(&s,commit.password,true,commit_password,&commit));
 assert(ph_auth_pair_unpair_request(&s,"peer",token,"192.0.2.1",nonce));
 commit.fail=false;assert(ph_auth_credential_commit(&s,commit.password,true,commit_password,&commit));
 assert(!ph_auth_pair_unpair_request(&s,"peer",token,"192.0.2.1",nonce)); /* same-password restore */
 /* A bounded receipt set admits a fifth success by evicting the oldest. */
 char first[33]={0};for(unsigned i=0;i<PH_AUTH_UNPAIR_RECEIPTS+1;i++){
   char peer[13];snprintf(peer,sizeof(peer),"receipt%u",i);
   assert(ph_auth_pair(&s,"adminfjr",peer,1,key)&&ph_auth_pair_resume(&s,peer,key,token));
   if(!i)strcpy(first,token);
   assert(ph_auth_pair_unpair_request(&s,peer,token,"192.0.2.1",nonce));f.now++;
 }
 assert(!ph_auth_pair_unpair_request(&s,"receipt0",first,"192.0.2.1",nonce));
 assert(ph_auth_pair_unpair_request(&s,"receipt4",token,"192.0.2.1",nonce));
}
static void test_inbound_only_store_migration(void){
 fixture f;ph_auth_state state;setup(&f,&state);char key[33];
 assert(ph_auth_pair(&state,"","001122334455",2,key));
 ph_auth_store expected=state.store,legacy=expected;
 const size_t prefix=offsetof(ph_auth_store,remote_count);
 memset((uint8_t*)&legacy+prefix,255,sizeof(legacy)-prefix);
 ph_auth_store frozen=legacy;
 assert(!ph_auth_store_valid(&legacy)&&ph_auth_store_migrate_legacy(&legacy));
 assert(!memcmp(&legacy,&frozen,prefix)&&!memcmp(&legacy,&expected,sizeof(expected)));
 assert(ph_auth_store_valid(&legacy)&&!ph_auth_store_migrate_legacy(&legacy));
 // Future headers, damaged prefix records and every non-erased extension byte
 // remain byte-for-byte untouched, including plausible mixed/partial writes.
 legacy=frozen;legacy.version++;ph_auth_store before=legacy;
 assert(!ph_auth_store_migrate_legacy(&legacy)&&!memcmp(&legacy,&before,sizeof(before)));
 legacy=frozen;legacy.pairs[0].scopes=128;before=legacy;
 assert(!ph_auth_store_migrate_legacy(&legacy)&&!memcmp(&legacy,&before,sizeof(before)));
 for(size_t i=prefix;i<sizeof(legacy);i++){
  legacy=frozen;((uint8_t*)&legacy)[i]=0;before=legacy;
  assert(!ph_auth_store_migrate_legacy(&legacy)&&!memcmp(&legacy,&before,sizeof(before)));
 }
}
int main(void){test_inbound_only_store_migration();test_unpair_receipts();test_pair_compaction_revokes_sessions();test_remote_direction_and_revocation();test_stateless_http_epoch();test_atomic_credential_commit();test_failed_password_transaction();test_unset_http();test_basic();test_expiry();test_capacity_failure();test_boot_revoke_mqtt();test_pending_pair_epoch();test_pair_retry();test_concurrent();puts("local_auth: all tests passed");return 0;}

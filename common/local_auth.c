#include "local_auth.h"
#include <string.h>
static void ph_auth_private_lock(ph_auth_state *s) {
    char p[33]={0};
    if(s->ops.lock)s->ops.lock(s->ops.user);
    if(s->ops.password(s->ops.user,p)&&memchr(p,0,sizeof(p))&&strcmp(p,s->credential)) {
        memset(s->http,0,sizeof(s->http));
        memset(s->local,0,sizeof(s->local));
        ph_auth_store_empty(&s->store);
        if(!++s->credential_epoch)++s->credential_epoch;
        memcpy(s->credential,p,sizeof(p));
    }
    memset(p,0,sizeof(p));
}
static void ph_auth_private_unlock(ph_auth_state *s) {
    if(s->ops.unlock)s->ops.unlock(s->ops.user);
}
static bool ph_auth_private_eq(const void *a,const void *b,size_t n) {
    const uint8_t*x=(const uint8_t*)a,*y=(const uint8_t*)b;
    unsigned d=0;
    while(n--)d|=*x++^*y++;
    return d==0;
}
static bool ph_auth_private_peer_ok(const char*p) {
    size_t n=p?strlen(p):0;
    return n>0&&n<13;
}
static bool ph_auth_private_nonzero(const uint8_t*p) {
    unsigned v=0;
    unsigned i;
    for(i=0;i<16;i++)v|=p[i];
    return v!=0;
}
bool ph_auth_token_decode(const char*p,uint8_t out[16]) {
    unsigned i;
    if(!p||strlen(p)!=32)return false;
    for(i=0;i<16;i++) {
        unsigned a=(unsigned char)p[2*i],b=(unsigned char)p[2*i+1];
        a=a>='0'&&a<='9'?a-'0':a>='a'&&a<='f'?a-'a'+10:255;
        b=b>='0'&&b<='9'?b-'0':b>='a'&&b<='f'?b-'a'+10:255;
        if(a>15||b>15)return false;
        out[i]=(uint8_t)((a<<4)|b);
    }
    return ph_auth_private_nonzero(out);
}
static void ph_auth_private_hex(const uint8_t*p,char*out) {
    static const char h[]="0123456789abcdef";
    unsigned i;
    for(i=0;i<16;i++) {
        out[i*2]=h[p[i]>>4];
        out[i*2+1]=h[p[i]&15];
    }
    out[32]=0;
}
void ph_auth_store_empty(ph_auth_store*p) {
    memset(p,0,sizeof(*p));
    p->magic=PH_AUTH_STORE_MAGIC;
    p->version=PH_AUTH_STORE_VERSION;
}
bool ph_auth_store_valid(const ph_auth_store*p) {
    unsigned i,j;
    if(!p||p->magic!=PH_AUTH_STORE_MAGIC||p->version!=PH_AUTH_STORE_VERSION||p->count>PH_AUTH_PAIR_SLOTS)return false;
    for(i=0;i<p->count;i++) {
        const ph_auth_pair_record*r=&p->pairs[i];
        if(!memchr(r->peer,0,sizeof(r->peer))||!ph_auth_private_peer_ok(r->peer)||!ph_auth_private_nonzero(r->key)||!r->scopes||(r->scopes&~(PH_AUTH_SCOPE_CONTROL|PH_AUTH_SCOPE_SENSOR|PH_AUTH_SCOPE_GROUP))||r->reserved[0]||r->reserved[1])return false;
        for(j=0;j<i;j++)if(!strcmp(p->pairs[j].peer,r->peer))return false;
    }
    return true;
}
bool ph_auth_init(ph_auth_state*s,const ph_auth_ops*o,const ph_auth_store*p) {
    if(!s||!o||!o->now||!o->random||!o->password||((o->lock==0)!=(o->unlock==0))||(p&&!ph_auth_store_valid(p)))return false;
    memset(s,0,sizeof(*s));
    s->ops=*o;
    s->credential_epoch=1;
    if(p)s->store=*p;
    else ph_auth_store_empty(&s->store);
    if(!o->password(o->user,s->credential)||!memchr(s->credential,0,sizeof(s->credential)))return false;
    return true;
}
static bool ph_auth_private_password_ok(ph_auth_state*s,const char*p) {
    char actual[33]={0};
    size_t n;
    bool ok;
    if(!p||strlen(p)>32||!s->ops.password(s->ops.user,actual)||!memchr(actual,0,sizeof(actual)))return false;
    n=strlen(actual);
    ok=strlen(p)==n&&ph_auth_private_eq(actual,p,n);
    memset(actual,0,sizeof(actual));
    return ok;
}
static bool ph_auth_private_alive(ph_auth_session*r,uint32_t now) {
    if(!r->active)return false;
    if((uint32_t)(now-r->last)>=PH_AUTH_IDLE_MS||(uint32_t)(now-r->born)>=PH_AUTH_MAX_MS) {
        memset(r,0,sizeof(*r));
        return false;
    }
    return true;
}
static bool ph_auth_private_fresh(ph_auth_state*s,uint8_t t[16]) {
    unsigned i,j;
    if(!s->ops.random(s->ops.user,t,16)||!ph_auth_private_nonzero(t))return false;
    for(i=0;i<PH_AUTH_HTTP_SLOTS;i++)if(s->http[i].active&&ph_auth_private_eq(t,s->http[i].token,16))return false;
    for(i=0;i<PH_AUTH_PAIR_SLOTS;i++) {
        if(s->local[i].active&&ph_auth_private_eq(t,s->local[i].token,16))return false;
        if(i<s->store.count&&ph_auth_private_eq(t,s->store.pairs[i].key,16))return false;
    }
    for(j=0;j<PH_AUTH_MQTT_SLOTS;j++)if(s->mqtt[j].active&&ph_auth_private_eq(t,s->mqtt[j].token,16))return false;
    return true;
}
static void ph_auth_private_install(ph_auth_state*s,ph_auth_session*r,const uint8_t*t,const char*peer,unsigned scopes) {
    memset(r,0,sizeof(*r));
    memcpy(r->token,t,16);
    r->born=r->last=s->ops.now(s->ops.user);
    r->generation=++s->generation;
    if(!r->generation)r->generation=++s->generation;
    r->active=1;
    r->scopes=(uint8_t)scopes;
    if(peer)strcpy(r->peer,peer);
}
ph_auth_login_result ph_auth_http_login_result(ph_auth_state*s,const char*p,char out[33]) {
    unsigned i;
    uint8_t t[16];
    ph_auth_login_result result=PH_AUTH_LOGIN_DENIED;
    ph_auth_private_lock(s);
    if(ph_auth_private_password_ok(s,p)) {
        result=PH_AUTH_LOGIN_BUSY;
        if(!ph_auth_private_fresh(s,t))goto done;
        uint32_t now=s->ops.now(s->ops.user);
        for(i=0;i<PH_AUTH_HTTP_SLOTS;i++)if(!ph_auth_private_alive(&s->http[i],now)) {
            ph_auth_private_install(s,&s->http[i],t,0,PH_AUTH_SCOPE_ALL);
            ph_auth_private_hex(t,out);
            result=PH_AUTH_LOGIN_OK;
            break;
        }
    }
done:
    memset(t,0,sizeof(t));
    ph_auth_private_unlock(s);
    return result;
}
bool ph_auth_http_login(ph_auth_state*s,const char*p,char out[33]) {
    return ph_auth_http_login_result(s,p,out)==PH_AUTH_LOGIN_OK;
}
bool ph_auth_password_is_set(ph_auth_state*s) {
    ph_auth_private_lock(s);
    bool set=s->credential[0]!=0;
    ph_auth_private_unlock(s);
    return set;
}
static void ph_auth_private_ctx(ph_auth_state*s,ph_auth_context*c,ph_auth_source source,unsigned slot,ph_auth_session*r) {
    memset(c,0,sizeof(*c));
    c->owner=s;
    c->source=source;
    c->slot=(uint8_t)slot;
    c->generation=r->generation;
    c->scopes=r->scopes;
    memcpy(c->peer,r->peer,sizeof(c->peer));
}
static int ph_auth_private_http_find(ph_auth_state*s,const char*a) {
    uint8_t t[16];
    unsigned i;
    if(!a||strncmp(a,"Bearer ",7)||!ph_auth_token_decode(a+7,t))return -1;
    for(i=0;i<PH_AUTH_HTTP_SLOTS;i++)if(ph_auth_private_alive(&s->http[i],s->ops.now(s->ops.user))&&ph_auth_private_eq(t,s->http[i].token,16))return (int)i;
    return -1;
}
bool ph_auth_http_check(ph_auth_state*s,const char*a,ph_auth_context*c) {
    int i;
    memset(c,0,sizeof(*c));
    ph_auth_private_lock(s);
    i=ph_auth_private_http_find(s,a);
    if(i>=0) {
        s->http[i].last=s->ops.now(s->ops.user);
        ph_auth_private_ctx(s,c,PH_AUTH_HTTP,(unsigned)i,&s->http[i]);
    }
    ph_auth_private_unlock(s);
    return i>=0;
}
bool ph_auth_http_logout(ph_auth_state*s,const char*a) {
    int i;
    ph_auth_private_lock(s);
    i=ph_auth_private_http_find(s,a);
    if(i>=0)memset(&s->http[i],0,sizeof(s->http[i]));
    ph_auth_private_unlock(s);
    return i>=0;
}
static bool ph_auth_private_save(ph_auth_state*s,const ph_auth_store*p) {
    if(!s->ops.save||!s->ops.save(s->ops.user,p))return false;
    s->store=*p;
    return true;
}
bool ph_auth_pair(ph_auth_state*s,const char*p,const char*peer,unsigned scopes,char out[33]) {
    ph_auth_store next;
    uint8_t t[16];
    unsigned i;
    bool ok=false;
    if(!ph_auth_private_peer_ok(peer)||!scopes||(scopes&~(PH_AUTH_SCOPE_CONTROL|PH_AUTH_SCOPE_SENSOR|PH_AUTH_SCOPE_GROUP)))return false;
    ph_auth_private_lock(s);
    if(ph_auth_private_password_ok(s,p)) {
        next=s->store;
        for(i=0;i<next.count;i++)if(!strcmp(peer,next.pairs[i].peer))break;
        /* Retrying a lost UDP reply must not invalidate an already returned key. */
        if(i<next.count&&next.pairs[i].scopes==scopes) {
            ph_auth_private_hex(next.pairs[i].key,out);
            ok=true;
        } else if(i<PH_AUTH_PAIR_SLOTS&&ph_auth_private_fresh(s,t)) {
            ph_auth_pair_record*r=&next.pairs[i];
            memset(r,0,sizeof(*r));
            strcpy(r->peer,peer);
            memcpy(r->key,t,16);
            r->scopes=(uint8_t)scopes;
            if(i==next.count)next.count++;
            if(ph_auth_private_save(s,&next)) {
                memset(&s->local[i],0,sizeof(s->local[i]));
                ph_auth_private_hex(t,out);
                ok=true;
            }
        }
    }
    ph_auth_private_unlock(s);
    return ok;
}
bool ph_auth_pair_resume(ph_auth_state*s,const char*peer,const char*key,char out[33]) {
    uint8_t k[16],t[16];
    unsigned i;
    bool ok=false;
    if(!ph_auth_private_peer_ok(peer)||!ph_auth_token_decode(key,k))return false;
    ph_auth_private_lock(s);
    for(i=0;i<s->store.count;i++)if(!strcmp(peer,s->store.pairs[i].peer)&&ph_auth_private_eq(k,s->store.pairs[i].key,16)) {
        /* Only reuse this boot's live local session after checking its durable key.
         * A handshake retry never extends the idle or absolute session lifetime. */
        ph_auth_session*r=&s->local[i];
        if(ph_auth_private_alive(r,s->ops.now(s->ops.user))&&
           !strcmp(r->peer,peer)&&r->scopes==s->store.pairs[i].scopes) {
            ph_auth_private_hex(r->token,out);
            ok=true;
        } else if(ph_auth_private_fresh(s,t)) {
            ph_auth_private_install(s,&s->local[i],t,peer,s->store.pairs[i].scopes);
            ph_auth_private_hex(t,out);
            ok=true;
        }
        break;
    }
    ph_auth_private_unlock(s);
    return ok;
}
bool ph_auth_pair_remove(ph_auth_state*s,const char*peer) {
    ph_auth_store next;
    unsigned i;
    bool ok=false;
    if(!ph_auth_private_peer_ok(peer))return false;
    ph_auth_private_lock(s);
    next=s->store;
    for(i=0;i<next.count;i++)if(!strcmp(peer,next.pairs[i].peer)) {
        memmove(&next.pairs[i],&next.pairs[i+1],(next.count-i-1)*sizeof(next.pairs[0]));
        memset(&next.pairs[--next.count],0,sizeof(next.pairs[0]));
        if(ph_auth_private_save(s,&next)) {
            memset(s->local,0,sizeof(s->local));
            ok=true;
        }
        break;
    }
    ph_auth_private_unlock(s);
    return ok;
}
bool ph_auth_pair_unpair(ph_auth_state*s,const char*peer,const char*token) {
    ph_auth_store next;uint8_t decoded[16];unsigned i,j;bool ok=false;
    if(!ph_auth_private_peer_ok(peer)||!ph_auth_token_decode(token,decoded))return false;
    ph_auth_private_lock(s);
    for(i=0;i<PH_AUTH_PAIR_SLOTS;i++) {
        ph_auth_session *r=&s->local[i];
        if(!ph_auth_private_alive(r,s->ops.now(s->ops.user))||strcmp(peer,r->peer)||
           !ph_auth_private_eq(decoded,r->token,16))continue;
        next=s->store;
        for(j=0;j<next.count;j++)if(!strcmp(peer,next.pairs[j].peer)) {
            memmove(&next.pairs[j],&next.pairs[j+1],(next.count-j-1)*sizeof(next.pairs[0]));
            memset(&next.pairs[--next.count],0,sizeof(next.pairs[0]));
            ok=ph_auth_private_save(s,&next);
            if(ok)memset(r,0,sizeof(*r));
            break;
        }
        break;
    }
    memset(decoded,0,sizeof(decoded));
    ph_auth_private_unlock(s);
    return ok;
}
bool ph_auth_udp_check(ph_auth_state*s,const char*peer,const char*token,ph_auth_context*c) {
    uint8_t t[16];
    unsigned i;
    bool ok=false;
    memset(c,0,sizeof(*c));
    if(!ph_auth_private_peer_ok(peer)||!ph_auth_token_decode(token,t))return false;
    ph_auth_private_lock(s);
    for(i=0;i<PH_AUTH_PAIR_SLOTS+PH_AUTH_MQTT_SLOTS;i++) {
        bool local=i<PH_AUTH_PAIR_SLOTS;
        unsigned slot=local?i:i-PH_AUTH_PAIR_SLOTS;
        ph_auth_session*r=local?&s->local[slot]:&s->mqtt[slot];
        if(ph_auth_private_alive(r,s->ops.now(s->ops.user))&&!strcmp(peer,r->peer)&&ph_auth_private_eq(t,r->token,16)) {
            r->last=s->ops.now(s->ops.user);
            ph_auth_private_ctx(s,c,local?PH_AUTH_UDP_PAIR:PH_AUTH_UDP_MQTT,slot,r);
            ok=true;
            break;
        }
    }
    ph_auth_private_unlock(s);
    return ok;
}
bool ph_auth_mqtt_grant(ph_auth_state*s,const char*peer,const char*token,unsigned scopes) {
    unsigned i,slot=PH_AUTH_MQTT_SLOTS;
    uint8_t t[16];
    bool ok=false;
    if(!ph_auth_private_peer_ok(peer)||!ph_auth_token_decode(token,t)||!scopes||(scopes&~(PH_AUTH_SCOPE_SENSOR|PH_AUTH_SCOPE_GROUP|PH_AUTH_SCOPE_CONTROL)))return false;
    ph_auth_private_lock(s);
    /* Refuse cross-channel token reuse. */for(i=0;i<PH_AUTH_HTTP_SLOTS;i++)if(s->http[i].active&&ph_auth_private_eq(t,s->http[i].token,16))goto done;
    for(i=0;i<PH_AUTH_PAIR_SLOTS;i++)if((s->local[i].active&&ph_auth_private_eq(t,s->local[i].token,16))||(i<s->store.count&&ph_auth_private_eq(t,s->store.pairs[i].key,16)))goto done;
    for(i=0;i<PH_AUTH_MQTT_SLOTS;i++) {
        if(s->mqtt[i].active&&!strcmp(peer,s->mqtt[i].peer)) {
            slot=i;
            break;
        }
        if(!ph_auth_private_alive(&s->mqtt[i],s->ops.now(s->ops.user)))slot=i;
    }
    if(slot<PH_AUTH_MQTT_SLOTS) {
        ph_auth_private_install(s,&s->mqtt[slot],t,peer,scopes);
        ok=true;
    }
    done:ph_auth_private_unlock(s);
    return ok;
}
void ph_auth_mqtt_context(ph_auth_state*s,ph_auth_context*c) {
    memset(c,0,sizeof(*c));
    c->owner=s;
    c->source=PH_AUTH_MQTT;
    c->scopes=PH_AUTH_SCOPE_ALL;
}
bool ph_auth_password_changed(ph_auth_state*s) {
    ph_auth_store next;
    bool ok;
    ph_auth_store_empty(&next);
    ph_auth_private_lock(s);
    ok=ph_auth_private_save(s,&next);
    /* This is the revoke phase BEFORE persisting a replacement password.
     * On failure the caller must retain the old durable password. Reboot may
     * reload the old pairs, so false must never be reported as revocation. */
    if(!ok)ph_auth_store_empty(&s->store);
    if(!++s->credential_epoch)++s->credential_epoch;
    memset(s->http,0,sizeof(s->http));
    memset(s->local,0,sizeof(s->local));
    ph_auth_private_unlock(s);
    return ok;
}
bool ph_auth_credential_commit(ph_auth_state*s,const char*password,bool force,
                               ph_auth_commit_fn commit,void*user) {
    ph_auth_store next;
    bool changed,ok;
    if(!s||!password||strlen(password)>32||!commit)return false;
    ph_auth_private_lock(s);
    changed=force||strcmp(password,s->credential)!=0;
    next=s->store;
    if(changed)ph_auth_store_empty(&next);
    ok=commit(user,&next);
    if(ok&&changed) {
        s->store=next;
        memset(s->http,0,sizeof(s->http));
        memset(s->local,0,sizeof(s->local));
        memset(s->credential,0,sizeof(s->credential));
        memcpy(s->credential,password,strlen(password));
        if(!++s->credential_epoch)++s->credential_epoch;
    }
    memset(&next,0,sizeof(next));
    ph_auth_private_unlock(s);
    return ok;
}
bool ph_auth_context_valid(const ph_auth_context*c,unsigned required) {
    ph_auth_state*s;
    ph_auth_session*r=0;
    bool ok=false;
    if(!c||!(s=c->owner)||!required||(required&~PH_AUTH_SCOPE_ALL))return false;
    ph_auth_private_lock(s);
    if(c->source==PH_AUTH_MQTT||c->source==PH_AUTH_INTERNAL) {
        ok=c->scopes==PH_AUTH_SCOPE_ALL;
    }
    else {
        if(c->source==PH_AUTH_HTTP&&c->slot<PH_AUTH_HTTP_SLOTS)r=&s->http[c->slot];
        if(c->source==PH_AUTH_UDP_PAIR&&c->slot<PH_AUTH_PAIR_SLOTS)r=&s->local[c->slot];
        if(c->source==PH_AUTH_UDP_MQTT&&c->slot<PH_AUTH_MQTT_SLOTS)r=&s->mqtt[c->slot];
        ok=r&&ph_auth_private_alive(r,s->ops.now(s->ops.user))&&r->generation==c->generation&&r->scopes==c->scopes&&!strcmp(r->peer,c->peer);
    }
    ok=ok&&((c->scopes&required)==required);
    ph_auth_private_unlock(s);
    return ok;
}
void ph_auth_local_revoke(ph_auth_state*s,const ph_auth_store*p) {
    ph_auth_private_lock(s);
    if(!++s->credential_epoch)++s->credential_epoch;
    memset(s->http,0,sizeof(s->http));
    memset(s->local,0,sizeof(s->local));
    if(p&&ph_auth_store_valid(p))s->store=*p;
    else ph_auth_store_empty(&s->store);
    ph_auth_private_unlock(s);
}
void ph_auth_internal_context(ph_auth_state*s,ph_auth_context*c) {
    ph_auth_mqtt_context(s,c);
    c->source=PH_AUTH_INTERNAL;
}
bool ph_auth_pair_store_epoch(ph_auth_state*s,const char*peer,const char*key,unsigned scopes,uint32_t expected_epoch) {
    ph_auth_store next;
    uint8_t t[16];
    unsigned i,j;
    bool ok=false;
    if(!ph_auth_private_peer_ok(peer)||!ph_auth_token_decode(key,t)||!scopes||(scopes&~(PH_AUTH_SCOPE_CONTROL|PH_AUTH_SCOPE_SENSOR|PH_AUTH_SCOPE_GROUP)))return false;
    ph_auth_private_lock(s);
    if(expected_epoch&&expected_epoch!=s->credential_epoch)goto done;
    next=s->store;
    for(j=0;j<PH_AUTH_HTTP_SLOTS;j++)if(s->http[j].active&&ph_auth_private_eq(t,s->http[j].token,16))goto done;
    for(j=0;j<PH_AUTH_MQTT_SLOTS;j++)if(s->mqtt[j].active&&ph_auth_private_eq(t,s->mqtt[j].token,16))goto done;
    for(j=0;j<PH_AUTH_PAIR_SLOTS;j++)if((s->local[j].active&&ph_auth_private_eq(t,s->local[j].token,16))||(j<next.count&&strcmp(peer,next.pairs[j].peer)&&ph_auth_private_eq(t,next.pairs[j].key,16)))goto done;
    for(i=0;i<next.count;i++)if(!strcmp(peer,next.pairs[i].peer))break;
    if(i<next.count&&next.pairs[i].scopes==scopes&&ph_auth_private_eq(next.pairs[i].key,t,16)) {
        ok=true;
        goto done;
    }
    if(i<PH_AUTH_PAIR_SLOTS) {
        ph_auth_pair_record*r=&next.pairs[i];
        memset(r,0,sizeof(*r));
        strcpy(r->peer,peer);
        memcpy(r->key,t,16);
        r->scopes=(uint8_t)scopes;
        if(i==next.count)next.count++;
        if(ph_auth_private_save(s,&next)) {
            memset(&s->local[i],0,sizeof(s->local[i]));
            ok=true;
        }
    }
    done:ph_auth_private_unlock(s);
    return ok;
}
bool ph_auth_pair_key(ph_auth_state*s,const char*peer,char out[33]) {
    unsigned i;
    bool ok=false;
    if(!ph_auth_private_peer_ok(peer))return false;
    ph_auth_private_lock(s);
    for(i=0;i<s->store.count;i++)if(!strcmp(peer,s->store.pairs[i].peer)) {
        ph_auth_private_hex(s->store.pairs[i].key,out);
        ok=true;
        break;
    }
    ph_auth_private_unlock(s);
    return ok;
}

uint32_t ph_auth_credential_epoch(ph_auth_state*s) {
    if(!s)return 0;
    ph_auth_private_lock(s);
    uint32_t epoch=s->credential_epoch;
    ph_auth_private_unlock(s);
    return epoch;
}

bool ph_auth_pair_store(ph_auth_state*s,const char*peer,const char*key,unsigned scopes) {
    return ph_auth_pair_store_epoch(s,peer,key,scopes,0);
}

bool ph_auth_pair_peer(ph_auth_state*s,unsigned index,char peer[13],unsigned*scopes) {
    if(!s||!peer||!scopes)return false;
    ph_auth_private_lock(s);
    bool found=index<s->store.count;
    if(found){memcpy(peer,s->store.pairs[index].peer,13);*scopes=s->store.pairs[index].scopes;}
    ph_auth_private_unlock(s);
    return found;
}

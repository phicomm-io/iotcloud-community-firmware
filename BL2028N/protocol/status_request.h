/* Allocation-free recognition of the frequent read-only status request.
 * This is deliberately not a general JSON parser. Anything outside this small
 * grammar (including escaped strings) falls back to the checked JSON parser. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct { const unsigned char *p,*end; } status_cursor;
typedef struct { bool live,has_id;char id[65]; } status_request_t;
static __attribute__((noinline)) void status_space(status_cursor *c)
{
    while(c->p<c->end && (*c->p==' ' || *c->p=='\t' || *c->p=='\n' || *c->p=='\r'))c->p++;
}
static __attribute__((noinline)) bool status_token(status_cursor *c,unsigned char token)
{
    status_space(c);
    if(c->p==c->end || *c->p!=token)return false;
    c->p++;return true;
}
static __attribute__((noinline)) bool status_word(status_cursor *c,const unsigned char **word,size_t *length)
{
    if(!status_token(c,'"'))return false;
    *word=c->p;
    while(c->p<c->end && *c->p!='"') {
        unsigned char v=*c->p++;
        if(!((v>='a'&&v<='z') || (v>='A'&&v<='Z') ||
             (v>='0'&&v<='9') || v=='_' || v=='.' || v=='-'))return false;
    }
    *length=(size_t)(c->p-*word);
    return status_token(c,'"');
}
static inline bool status_equal(const unsigned char *word,size_t length,const char *text)
{ return strlen(text)==length && !memcmp(word,text,length); }
static inline bool status_request_action_decode(const void *bytes,size_t length,status_request_t *out,const char *action)
{
    if(!bytes || !length || length>1024)return false;
    status_cursor c={(const unsigned char *)bytes,(const unsigned char *)bytes+length};
    status_request_t value={0};unsigned seen=0;
    if(!status_token(&c,'{'))return false;
    for(unsigned field=0;field<3;field++) {
        const unsigned char *key,*word;size_t n,m;
        if(!status_word(&c,&key,&n) || !status_token(&c,':'))return false;
        if(status_equal(key,n,"params")) {
            if(seen&2u || !status_token(&c,'{'))return false;
            seen|=2u;
            if(!status_token(&c,'}')) {
                if(strcmp(action,"status.get") || !status_word(&c,&word,&m) || !status_equal(word,m,"view") ||
                   !status_token(&c,':') || !status_word(&c,&word,&m))return false;
                if(status_equal(word,m,"live"))value.live=true;
                else if(!status_equal(word,m,"full"))return false;
                if(!status_token(&c,'}'))return false;
            }
        } else if(status_equal(key,n,"action")) {
            if(seen&1u || !status_word(&c,&word,&m) || !status_equal(word,m,action))return false;
            seen|=1u;
        } else if(status_equal(key,n,"request_id")) {
            if(seen&4u || !status_word(&c,&word,&m) || !m || m>=sizeof(value.id))return false;
            memcpy(value.id,word,m);value.id[m]=0;value.has_id=true;seen|=4u;
        } else return false;
        if(status_token(&c,'}')) {
            status_space(&c);
            if(c.p!=c.end || (seen&3u)!=3u)return false;
            *out=value;return true;
        }
        if(!status_token(&c,','))return false;
    }
    return false;
}

static inline bool status_request_decode(const void *bytes,size_t length,status_request_t *out)
{ return status_request_action_decode(bytes,length,out,"status.get"); }

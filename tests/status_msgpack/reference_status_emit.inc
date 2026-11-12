/* Private wire sink for the frozen status schema. No device calls or caches. */
#include "printbuf.h"
#include "json_heap.h"
#include "json_number_checked.h"
#include <limits.h>
#define STATUS_DEPTH 4
typedef struct {
    char *buf;
    size_t used,capacity;
    unsigned depth;
    bool failed,array[STATUS_DEPTH],first[STATUS_DEPTH];
} status_sink;
static void emit_bytes(status_sink *s,const char *text,size_t n)
{
    if(s->failed)return;
    if(n>(size_t)INT_MAX-s->used || (s->buf && n>s->capacity-s->used)) { s->failed=true;return; }
    if(s->buf)memcpy(s->buf+s->used,text,n);
    s->used+=n;
}
static void emit_quoted(status_sink *s,const char *text)
{
    static const char hex[]="0123456789abcdef";
    emit_bytes(s,"\"",1);
    for(const unsigned char *p=(const unsigned char *)text;*p && !s->failed;p++) {
        const char *escape=NULL;
        switch(*p) {
        case '\b':escape="\\b";break;case '\n':escape="\\n";break;
        case '\r':escape="\\r";break;case '\t':escape="\\t";break;
        case '"':escape="\\\"";break;case '\\':escape="\\\\";break;case '/':escape="\\/";break;
        }
        if(escape)emit_bytes(s,escape,2);
        else if(*p<32) { char code[6]={'\\','u','0','0',hex[*p>>4],hex[*p&15]};emit_bytes(s,code,6); }
        else emit_bytes(s,(const char *)p,1);
    }
    emit_bytes(s,"\"",1);
}
static void emit_prefix(status_sink *s,const char *key)
{
    if(!s->depth || s->failed)return;
    unsigned d=s->depth-1;
    emit_bytes(s,s->first[d]?" ":", ",s->first[d]?1:2);s->first[d]=false;
    if(!s->array[d]) {
        if(!key) { s->failed=true;return; }
        emit_quoted(s,key);emit_bytes(s,": ",2);
    } else if(key)s->failed=true;
}
static void emit_begin(status_sink *s,const char *key,bool array)
{
    if(s->failed)return;
    if(s->depth==STATUS_DEPTH) { s->failed=true;return; }
    emit_prefix(s,key);emit_bytes(s,array?"[":"{",1);
    s->array[s->depth]=array;s->first[s->depth]=true;s->depth++;
}
static void emit_end(status_sink *s)
{
    if(s->failed)return;
    if(!s->depth) { s->failed=true;return; }
    --s->depth;
    emit_bytes(s,s->array[s->depth]?" ]":" }",2);
}
static void emit_string(status_sink *s,const char *key,const char *text)
{ emit_prefix(s,key);emit_quoted(s,text); }
static void emit_int(status_sink *s,const char *key,int32_t n)
{
    char number[12];int used=snprintf(number,sizeof(number),"%ld",(long)n);
    emit_prefix(s,key);emit_bytes(s,number,(size_t)used);
}
static void emit_bool(status_sink *s,const char *key,bool value)
{ emit_prefix(s,key);emit_bytes(s,value?"true":"false",value?4:5); }
static void emit_null(status_sink *s,const char *key)
{ emit_prefix(s,key);emit_bytes(s,"null",4); }
static void emit_double(status_sink *s,const char *key,double value)
{
    if(s->failed)return;
    char number[32];int n=json_number_format(number,sizeof(number),value);
    if(n<0) { s->failed=true;return; }
    emit_prefix(s,key);emit_bytes(s,number,(size_t)n);
}
static void variant_fields(status_sink *o)
{
#define STATUS_INT(k,v) emit_int(o,k,v)
#define STATUS_BOOL(k,v) emit_bool(o,k,v)
#define STATUS_STRING(k,v) emit_string(o,k,v)
#include "variant_fields.inc"
#undef STATUS_INT
#undef STATUS_BOOL
#undef STATUS_STRING
}
#if defined(PHICOMM_APPLICATION_WATCHDOG)
static void watchdog_fields(status_sink *o,const watchdog_status_t *s)
{
#define STATUS_INT(k,v) emit_int(o,k,v)
#define STATUS_BOOL(k,v) emit_bool(o,k,v)
#define STATUS_STRING(k,v) emit_string(o,k,v)
#include "watchdog_fields.inc"
#undef STATUS_INT
#undef STATUS_BOOL
#undef STATUS_STRING
}
#endif

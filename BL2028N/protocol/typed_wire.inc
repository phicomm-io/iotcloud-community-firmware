/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "typed_wire.h"
#include "binary_reader.h"
#include "json_heap.h"
#include "json_number_checked.h"
#if PHICOMM_A1_DECIMAL
#include "a1_decimal.inc"
#define phw_json_format ph_decimal_format
#define phw_json_format_float32 ph_decimal_format_float32
#else
#define phw_json_format json_number_format
#define phw_json_format_float32 json_number_format_float32
#endif
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <math.h>
typedef struct {const unsigned char *p,*end;ph_wire_format format;unsigned nodes;} phv_reader;
static void phv_ws(phv_reader *r){while(r->p<r->end && (*r->p==' '||*r->p=='\n'||*r->p=='\r'||*r->p=='\t'))r->p++;}
static int phv_hex(unsigned c){return c>='0'&&c<='9'?(int)c-'0':c>='a'&&c<='f'?(int)c-'a'+10:c>='A'&&c<='F'?(int)c-'A'+10:-1;}
static bool phv_u16(const unsigned char **p,const unsigned char *end,unsigned *v){if(end-*p<4)return false;*v=0;for(unsigned i=0;i<4;i++){int h=phv_hex(*(*p)++);if(h<0)return false;*v=*v*16+(unsigned)h;}return true;}
/* JSON string decoder also validates escaped Unicode without temporary graphs. */
static bool phv_unquote(const unsigned char *p,const unsigned char *end,char *out,size_t cap,size_t *length){
 size_t n=0;while(p<end){unsigned c=*p++;unsigned char bytes[4];unsigned count=1;bytes[0]=(unsigned char)c;
  if(c=='\\'){if(p==end)return false;c=*p++;switch(c){case '"':case '\\':case '/':bytes[0]=(unsigned char)c;break;case 'b':bytes[0]='\b';break;case 'f':bytes[0]='\f';break;case 'n':bytes[0]='\n';break;case 'r':bytes[0]='\r';break;case 't':bytes[0]='\t';break;
   case 'u':{unsigned u;if(!phv_u16(&p,end,&u))return false;if(u>=0xd800&&u<=0xdbff){unsigned lo;if(end-p<6||p[0]!='\\'||p[1]!='u')return false;p+=2;if(!phv_u16(&p,end,&lo)||lo<0xdc00||lo>0xdfff)return false;u=0x10000+((u-0xd800)<<10)+(lo-0xdc00);}else if(u>=0xdc00&&u<=0xdfff)return false;
    if(!u)return false;if(u<128)bytes[0]=(unsigned char)u;else if(u<2048){count=2;bytes[0]=0xc0|(u>>6);bytes[1]=0x80|(u&63);}else if(u<65536){count=3;bytes[0]=0xe0|(u>>12);bytes[1]=0x80|((u>>6)&63);bytes[2]=0x80|(u&63);}else{count=4;bytes[0]=0xf0|(u>>18);bytes[1]=0x80|((u>>12)&63);bytes[2]=0x80|((u>>6)&63);bytes[3]=0x80|(u&63);}break;}
   default:return false;}
  }else if(c<32||c=='"')return false;
  if(out){if(n+count>=cap)return false;memcpy(out+n,bytes,count);}n+=count;
 }if(out){if(n>=cap)return false;out[n]=0;}if(length)*length=n;return true;
}
static bool phv_scan(phv_reader *,ph_value *,unsigned,bool);
static bool phv_json_string(phv_reader *r,ph_value *v){
 if(r->p==r->end||*r->p++!='"')return false;const unsigned char *p=r->p;bool escape=false;
 while(r->p<r->end){unsigned c=*r->p++;if(!escape&&c=='"'){v->text=p;v->size=(size_t)(r->p-1-p);v->kind=PHV_TEXT;return ph_wire_utf8(p,v->size)&&phv_unquote(p,r->p-1,NULL,0,NULL);}if(!escape&&c=='\\')escape=true;else escape=false;}return false;
}
static bool phv_skip_one(phv_reader *r,ph_value *v,unsigned depth){return phv_scan(r,v,depth,false);}
/* Primitive/header reader is a separate frame: the JSON number scratch buffer
 * is never multiplied by nesting depth. */
static __attribute__((noinline)) bool phv_head(phv_reader *r,ph_value *v){
 memset(v,0,sizeof(*v));v->kind=PHV_MISSING;v->format=r->format;
 if(++r->nodes>PH_BINARY_NODES)return false;if(!r->format)phv_ws(r);if(r->p==r->end)return false;v->begin=r->p;
 if(r->format){binary_reader b={.p=r->p,.end=r->end,.f=r->format};binary_item item={0};if(!binary_token(&b,&item))return false;r->p=b.p;v->kind=item.kind;if(item.kind==PHV_REAL)v->real=item.real;else v->count=item.n;v->text=item.text;v->size=(item.kind==PHV_TEXT||item.kind==PHV_BYTES)?(size_t)item.n:0;}
 else {
  unsigned c=*r->p;
  if(c=='"'){if(!phv_json_string(r,v))return false;}
  else if(c=='{'||c=='['){v->kind=c=='{'?PHV_MAP:PHV_ARRAY;r->p++;/* container header */;}
  else if(c=='t'&&r->end-r->p>=4&&!memcmp(r->p,"true",4)){v->kind=PHV_TRUE;r->p+=4;}
  else if(c=='f'&&r->end-r->p>=5&&!memcmp(r->p,"false",5)){v->kind=PHV_FALSE;r->p+=5;}
  else if(c=='n'&&r->end-r->p>=4&&!memcmp(r->p,"null",4)){v->kind=PHV_NULL;r->p+=4;}
  else {
   const unsigned char *start=r->p;bool neg=false,real=false;if(*r->p=='-'){neg=true;if(++r->p==r->end)return false;}
   if(*r->p=='0')r->p++;else {if(*r->p<'1'||*r->p>'9')return false;while(r->p<r->end&&*r->p>='0'&&*r->p<='9')r->p++;}
   if(r->p<r->end&&*r->p=='.'){real=true;r->p++;const unsigned char *digits=r->p;while(r->p<r->end&&*r->p>='0'&&*r->p<='9')r->p++;if(r->p==digits)return false;}
   if(r->p<r->end&&(*r->p=='e'||*r->p=='E')){real=true;r->p++;if(r->p<r->end&&(*r->p=='+'||*r->p=='-'))r->p++;const unsigned char *digits=r->p;while(r->p<r->end&&*r->p>='0'&&*r->p<='9')r->p++;if(r->p==digits)return false;}
   if(real){char text[351];size_t n=(size_t)(r->p-start);if(n>=sizeof(text))return false;memcpy(text,start,n);text[n]=0;
#if PHICOMM_A1_DECIMAL
    if(ph_decimal_scan(text,&v->real)!=1)return false;
#elif defined(__NEWLIB__)
    if(json_number_scan(text,&v->real)!=1)return false;
#else
    char *last;v->real=strtod(text,&last);if(*last)return false;
#endif
    if(!isfinite(v->real))return false;v->kind=PHV_REAL;
   }else{uint64_t n=0;for(const unsigned char *p=start+neg;p<r->p;p++){unsigned digit=*p-'0';if(n>(PH_BINARY_SAFE-digit)/10)return false;n=n*10+digit;}v->kind=neg&&n?PHV_NEG:PHV_UINT;v->count=neg&&n?n-1:n;}
  }
 }
 v->end=r->p;return true;
}
typedef struct {const unsigned char *children;uint32_t bloom[2];uint16_t index;uint16_t limit;unsigned kind;} phv_frame;
/* Only the low six hash bits select the same 64-bit bloom position. The
 * low 32-bit FNV arithmetic is equivalent and avoids 64-bit multiply code. */
static uint32_t phv_key_hash(const char *key){uint32_t h=(uint32_t)UINT64_C(1469598103934665603);while(*key){h^=(unsigned char)*key++;h*=UINT32_C(435);}return h;}
/* Only a bloom hit scans earlier keys, and that scan never validates nested
 * keys again. Common schema maps are linear; a shared comparison budget makes
 * adversarial collision inputs bounded without a heap token table. */
/* Duplicate detection only revisits values already validated by the primary
 * scanner. Skip that immutable prefix without another nesting-frame array or
 * JSON numeric conversion; no unvalidated input uses this helper. */
static __attribute__((noinline)) bool phv_skip_validated(phv_reader *r,ph_value *v){
 memset(v,0,sizeof(*v));v->format=r->format;v->begin=r->p;
 if(r->format){
  binary_reader b={.p=r->p,.end=r->end,.f=r->format};binary_item item={0};
  if(!binary_token(&b,&item))return false;v->kind=item.kind;v->text=item.text;v->size=(item.kind==PHV_TEXT||item.kind==PHV_BYTES)?(size_t)item.n:0;
  unsigned pending=item.kind==PHV_MAP?(unsigned)item.n*2:item.kind==PHV_ARRAY?(unsigned)item.n:0;
  while(pending){if(!binary_token(&b,&item))return false;pending--;if(item.kind==PHV_MAP)pending+=(unsigned)item.n*2;else if(item.kind==PHV_ARRAY)pending+=(unsigned)item.n;}
  r->p=b.p;
 }else{
  phv_ws(r);v->begin=r->p;if(r->p==r->end)return false;
  if(*r->p=='"'){if(!phv_json_string(r,v))return false;}
  else if(*r->p=='{'||*r->p=='['){
   unsigned depth=0;bool quoted=false,escaped=false;
   do {unsigned c=*r->p++;if(quoted){if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c=='"')quoted=false;}
    else if(c=='"')quoted=true;else if(c=='{'||c=='[')depth++;else if(c=='}'||c==']')depth--;
   }while(depth&&r->p<r->end);
   if(depth)return false;
  }else {while(r->p<r->end&&*r->p!=','&&*r->p!='}'&&*r->p!=']'&&*r->p!=' '&&*r->p!='\n'&&*r->p!='\r'&&*r->p!='\t')r->p++;}
 }
 v->end=r->p;return true;
}
static __attribute__((noinline)) bool phv_duplicate(phv_reader *r,const phv_frame *frame,const char *name,unsigned *budget){
 phv_reader previous={frame->children,r->p,r->format,0};
 for(unsigned j=0;j<frame->index;j++){
  if(!*budget)return true;(*budget)--;ph_value key,value;
  if(!r->format){phv_ws(&previous);if(j){if(previous.p==previous.end||*previous.p++!=',')return true;}}
  if(!phv_skip_validated(&previous,&key)||phv_equal_text(&key,name))return true;
  if(!r->format){phv_ws(&previous);if(previous.p==previous.end||*previous.p++!=':')return true;}
  if(!phv_skip_validated(&previous,&value))return true;
 }
 return false;
}
static bool phv_scan(phv_reader *r,ph_value *v,unsigned depth,bool validate){
 phv_frame stack[PH_BINARY_DEPTH+1];unsigned used=0,budget=PH_BINARY_NODES*8;ph_value item;
 if(depth>PH_BINARY_DEPTH||!phv_head(r,v))return false;
 if(v->kind!=PHV_ARRAY&&v->kind!=PHV_MAP)return true;
 stack[used++]=(phv_frame){.children=r->p,.limit=(uint16_t)v->count,.kind=v->kind};
 while(used){
  phv_frame *f=&stack[used-1];bool done=false;
  if(!r->format){phv_ws(r);if(r->p==r->end)return false;unsigned close=f->kind==PHV_MAP?'}':']';
   if(*r->p==close){r->p++;done=true;}else if(f->index){if(*r->p++!=',')return false;phv_ws(r);}
  }else done=f->index==f->limit;
  if(done){if(used==1)v->count=f->index;used--;continue;}
  if(f->index>=PH_BINARY_NODES)return false;
  if(f->kind==PHV_MAP){
   if(!phv_head(r,&item)||item.kind!=PHV_TEXT)return false;char key[128];if(!phv_text(&item,key,sizeof(key)))return false;
   if(validate){unsigned hash=(unsigned)(phv_key_hash(key)&63),word=hash>>5;uint32_t bit=UINT32_C(1)<<(hash&31);if((f->bloom[word]&bit)&&phv_duplicate(r,f,key,&budget))return false;f->bloom[word]|=bit;}
   if(!r->format){phv_ws(r);if(r->p==r->end||*r->p++!=':')return false;}
  }
  if(!phv_head(r,&item))return false;f->index++;
  if(item.kind==PHV_ARRAY||item.kind==PHV_MAP){if(depth+used>PH_BINARY_DEPTH||used>=PH_BINARY_DEPTH+1)return false;stack[used++]=(phv_frame){.children=r->p,.limit=(uint16_t)item.count,.kind=item.kind};}
 }
 v->end=r->p;return true;
}

bool phv_parse(ph_wire_format format,const void *bytes,size_t size,ph_value *out){if(!out)return false;memset(out,0,sizeof(*out));out->kind=PHV_MISSING;if(!bytes||!size||size>PH_BINARY_BYTES||(format!=PH_WIRE_MSGPACK&&format!=PH_WIRE_CBOR))return false;phv_reader r={bytes,(const unsigned char *)bytes+size,format,0};if(!phv_scan(&r,out,0,true))return false;if(!format)phv_ws(&r);return r.p==r.end;}
bool phv_bytes(const ph_value *v,const void **bytes,size_t *size){if(!v||v->kind!=PHV_BYTES||!bytes||!size)return false;*bytes=v->text;*size=v->size;return true;}
bool phv_text(const ph_value *v,char *text,size_t capacity){if(!v||v->kind!=PHV_TEXT||!text||!capacity)return false;if(!v->format)return phv_unquote(v->text,v->text+v->size,text,capacity,NULL);if(v->size>=capacity)return false;memcpy(text,v->text,v->size);text[v->size]=0;return true;}
bool phv_equal_text(const ph_value *v,const char *text){
 if(!v||v->kind!=PHV_TEXT||!text)return false;if(v->format)return strlen(text)==v->size&&!memcmp(v->text,text,v->size);
 const unsigned char *p=v->text,*end=p+v->size;size_t at=0,n=strlen(text);
 while(p<end){if(*p!='\\'){if(at==n||text[at++]!=(char)*p++)return false;continue;}
  const unsigned char *start=p++;if(p==end)return false;unsigned c=*p++;if(c=='u'){if(end-p<4)return false;unsigned u;const unsigned char *peek=p;if(!phv_u16(&peek,end,&u))return false;p=peek;if(u>=0xd800&&u<=0xdbff){if(end-p<6)return false;p+=6;}}
  char part[5];size_t count;if(!phv_unquote(start,p,part,sizeof(part),&count)||count>n-at||memcmp(text+at,part,count))return false;at+=count;
 }return at==n;
}
static bool phv_child_reader(const ph_value *v,phv_reader *r){if(!v||(v->kind!=PHV_MAP&&v->kind!=PHV_ARRAY))return false;*r=(phv_reader){v->begin,v->end,v->format,0};if(v->format){binary_reader b={.p=r->p,.end=r->end,.f=r->format};binary_item item={0};if(!binary_token(&b,&item))return false;r->p=b.p;}else r->p++;return true;}
bool phv_iter(const ph_value *v,ph_iterator *it){phv_reader r;if(!it||!phv_child_reader(v,&r))return false;*it=(ph_iterator){r.p,r.end,v->format,v->kind,0,(unsigned)v->count};return true;}
bool phv_next(ph_iterator *it,char key[128],ph_value *out){if(!it||!out||it->index>=it->count)return false;phv_reader r={it->cursor,it->end,it->format,0};if(!it->format){phv_ws(&r);if(it->index){if(r.p==r.end||*r.p++!=',')return false;}}
 if(it->kind==PHV_MAP){ph_value k;if(!phv_skip_one(&r,&k,0)||k.kind!=PHV_TEXT)return false;if(key&&!phv_text(&k,key,128))return false;if(!it->format){phv_ws(&r);if(r.p==r.end||*r.p++!=':')return false;}}
 if(!phv_skip_one(&r,out,0))return false;it->cursor=r.p;it->index++;return true;}
bool phv_pair(const ph_value *v,unsigned index,char key[128],ph_value *out){if(out){memset(out,0,sizeof(*out));out->kind=PHV_MISSING;}ph_iterator it;if(!out||!v||v->kind!=PHV_MAP||index>=v->count||!phv_iter(v,&it))return false;for(unsigned i=0;i<=index;i++)if(!phv_next(&it,i==index?key:NULL,out))return false;return true;}
bool phv_get(const ph_value *v,const char *name,ph_value *out){if(!out)return false;memset(out,0,sizeof(*out));out->kind=PHV_MISSING;ph_iterator it;if(!v||v->kind!=PHV_MAP||!phv_iter(v,&it))return false;while(it.index<it.count){char key[128];ph_value value;if(!phv_next(&it,key,&value))return false;if(!strcmp(key,name)){*out=value;return true;}}return false;}

bool phv_at(const ph_value *v,unsigned index,ph_value *out){if(!out)return false;memset(out,0,sizeof(*out));out->kind=PHV_MISSING;phv_reader r;if(!v||v->kind!=PHV_ARRAY||index>=v->count||!phv_child_reader(v,&r))return false;for(unsigned i=0;i<=index;i++){if(!v->format){phv_ws(&r);if(i){if(r.p==r.end||*r.p++!=',')return false;}}if(!phv_skip_one(&r,out,0))return false;}return true;}
bool phv_int(const ph_value *v,int64_t *out){if(!v||!out||(v->kind!=PHV_UINT&&v->kind!=PHV_NEG))return false;*out=v->kind==PHV_NEG?-1-(int64_t)v->count:(int64_t)v->count;return true;}
bool phv_number(const ph_value *v,double *out){int64_t n;if(!v||!out)return false;if(v->kind==PHV_REAL){*out=v->real;return true;}if(!phv_int(v,&n))return false;*out=(double)n;return true;}
bool phv_bool(const ph_value *v,bool *out){if(!v||!out||(v->kind!=PHV_FALSE&&v->kind!=PHV_TRUE))return false;*out=v->kind==PHV_TRUE;return true;}
bool phv_keys(const ph_value *v,const char *allowed){ph_iterator it;if(!v||v->kind!=PHV_MAP||!phv_iter(v,&it))return false;while(it.index<it.count){char key[128],needle[130];ph_value item;if(!phv_next(&it,key,&item))return false;size_t n=strlen(key);needle[0]='|';memcpy(needle+1,key,n);needle[n+1]='|';needle[n+2]=0;if(!strstr(allowed,needle))return false;}return true;}

static void phw_bytes(ph_writer *w,const void *data,size_t n){
 if(w->failed)return;if(!w->wire||n>PHW_MAX_BYTES-(size_t)w->wire->bpos){w->failed=true;return;}size_t need=(size_t)w->wire->bpos+n+1;
 if(need>(size_t)w->wire->size){if(w->fixed){w->failed=true;return;}size_t cap=(size_t)w->wire->size;while(cap<need){size_t next=cap+cap/2+64;cap=next>PHW_MAX_BYTES+1?PHW_MAX_BYTES+1:next;if(cap<need&&cap==PHW_MAX_BYTES+1){w->failed=true;return;}}char *p=json_heap_resize(w->wire->buf,(size_t)w->wire->size,cap);if(!p){w->failed=true;return;}w->wire->buf=p;w->wire->size=(int)cap;}
 if(w->wire->buf)memcpy(w->wire->buf+w->wire->bpos,data,n);w->wire->bpos+=(int)n;if(w->wire->buf)w->wire->buf[w->wire->bpos]=0;
}
bool phw_init_capacity(ph_writer *w,ph_wire_format format,size_t capacity){memset(w,0,sizeof(*w));w->format=format;if((format!=PH_WIRE_MSGPACK&&format!=PH_WIRE_CBOR)||!capacity||capacity>PHW_MAX_BYTES+1){w->failed=true;return false;}w->wire=json_heap_calloc(1,sizeof(*w->wire));if(!w->wire){w->failed=true;return false;}w->wire->size=(int)capacity;w->wire->buf=json_heap_malloc(capacity);if(!w->wire->buf){printbuf_free(w->wire);w->wire=NULL;w->failed=true;return false;}w->wire->buf[0]=0;return true;}
bool phw_init(ph_writer *w,ph_wire_format format){return phw_init_capacity(w,format,128);}
bool phw_init_measure(ph_writer *w,ph_wire_format format,struct printbuf *wire){memset(w,0,sizeof(*w));w->fixed=true;w->format=format;if(!wire||(format!=PH_WIRE_MSGPACK&&format!=PH_WIRE_CBOR)){w->failed=true;return false;}memset(wire,0,sizeof(*wire));wire->size=PHW_MAX_BYTES+1;w->wire=wire;return true;}
bool phw_init_fixed(ph_writer *w,ph_wire_format format,struct printbuf *wire,void *buffer,size_t size){memset(w,0,sizeof(*w));w->fixed=true;w->format=format;if(!wire||!buffer||!size||size>INT_MAX||(format!=PH_WIRE_MSGPACK&&format!=PH_WIRE_CBOR)){w->failed=true;return false;}memset(wire,0,sizeof(*wire));wire->buf=buffer;wire->size=(int)size;wire->buf[0]=0;w->wire=wire;return true;}
size_t phw_finish_fixed(ph_writer *w){return w->fixed&&!w->failed&&!w->depth&&!w->root_key&&w->wire?(size_t)w->wire->bpos:0;}
static void phw_head(ph_writer *w,unsigned kind,uint64_t n){unsigned char bytes[9];size_t count=ph_wire_head(w->format,kind,n,bytes);phw_bytes(w,bytes,count);}
static void phw_quote(ph_writer *w,const char *text,size_t size){if(!ph_wire_utf8((const unsigned char *)text,size)){w->failed=true;return;}if(w->format){phw_head(w,3,size);phw_bytes(w,text,size);return;}static const char hex[]="0123456789abcdef";phw_bytes(w,"\"",1);for(size_t i=0;i<size;i++){unsigned c=(unsigned char)text[i];if(c=='"'||c=='\\'){char b[2]={'\\',(char)c};phw_bytes(w,b,2);}else if(c<32){char b[6]={'\\','u','0','0',hex[c>>4],hex[c&15]};phw_bytes(w,b,6);}else phw_bytes(w,text+i,1);}phw_bytes(w,"\"",1);}
void phw_root_key(ph_writer *w,const char *key){if(w->root_key||!key||!w->depth||w->arrays[w->depth-1])w->failed=true;else w->root_key=key;}
static void phw_prefix(ph_writer *w,const char *key){if(w->failed)return;if(w->root_key){if(key){w->failed=true;return;}key=w->root_key;w->root_key=NULL;}if(!w->depth){if(key||!w->wire||w->wire->bpos)w->failed=true;return;}unsigned d=w->depth-1;if(w->counts[d]==UINT16_MAX){w->failed=true;return;}if(!w->format&&w->counts[d])phw_bytes(w,",",1);w->counts[d]++;if(!w->arrays[d]){if(!key){w->failed=true;return;}phw_quote(w,key,strlen(key));if(!w->format)phw_bytes(w,":",1);}else if(key)w->failed=true;}
void phw_begin(ph_writer *w,const char *key,bool array){phw_prefix(w,key);if(w->failed)return;if(w->depth>=PHW_DEPTH){w->failed=true;return;}unsigned d=w->depth++;w->arrays[d]=array;w->counts[d]=0;w->starts[d]=(size_t)w->wire->bpos;if(w->format){unsigned char b[3]={w->format==PH_WIRE_CBOR?(array?0x99:0xb9):(array?0xdc:0xde),0,0};phw_bytes(w,b,3);}else phw_bytes(w,array?"[":"{",1);}
void phw_end(ph_writer *w){if(w->failed)return;if(!w->depth){w->failed=true;return;}unsigned d=--w->depth;if(w->format){size_t p=w->starts[d];if(w->wire->buf){w->wire->buf[p+1]=(char)(w->counts[d]>>8);w->wire->buf[p+2]=(char)w->counts[d];}}else phw_bytes(w,w->arrays[d]?"]":"}",1);}
void phw_text_n(ph_writer *w,const char *key,const char *text,size_t size){phw_prefix(w,key);if(!text){w->failed=true;return;}phw_quote(w,text,size);}
void phw_bytes_value(ph_writer *w,const char *key,const void *bytes,size_t size){phw_prefix(w,key);if(!w->format||(!bytes&&size)){w->failed=true;return;}phw_head(w,PHV_BYTES,size);phw_bytes(w,bytes,size);}
void phw_text(ph_writer *w,const char *key,const char *text){if(!text){w->failed=true;return;}phw_text_n(w,key,text,strlen(text));}
static void phw_digits(ph_writer *w,uint64_t n,bool negative){char b[24];unsigned count=0;do{b[count++]=(char)('0'+n%10);n/=10;}while(n);if(negative)b[count++]='-';for(unsigned i=0;i<count/2;i++){char c=b[i];b[i]=b[count-1-i];b[count-1-i]=c;}phw_bytes(w,b,count);}
void phw_uint(ph_writer *w,const char *key,uint64_t n){phw_prefix(w,key);if(n>PH_BINARY_SAFE){w->failed=true;return;}if(w->format)phw_head(w,0,n);else phw_digits(w,n,false);}
void phw_int(ph_writer *w,const char *key,int64_t n){if(n>=0){phw_uint(w,key,(uint64_t)n);return;}phw_prefix(w,key);uint64_t magnitude=(uint64_t)(-(n+1));if(magnitude>=PH_BINARY_SAFE){w->failed=true;return;}if(w->format)phw_head(w,1,magnitude);else phw_digits(w,magnitude+1,true);}
/* Share ARM argument extension for scalar schemas with known 32-bit widths. */
void __attribute__((noinline)) phw_i32(ph_writer *w,const char *key,int32_t n){phw_int(w,key,n);}
void __attribute__((noinline)) phw_u32(ph_writer *w,const char *key,uint32_t n){phw_uint(w,key,n);}
void phw_number(ph_writer *w,const char *key,double n){phw_prefix(w,key);if(!isfinite(n)){w->failed=true;return;}if(w->format){unsigned char b[9];size_t length=ph_wire_double(w->format,n,b);phw_bytes(w,b,length);}else{char b[32];int length=phw_json_format(b,sizeof(b),n);if(length<0){w->failed=true;return;}phw_bytes(w,b,(size_t)length);}}
void phw_float32(ph_writer *w,const char *key,float n){if(w->format){phw_number(w,key,(double)n);return;}phw_prefix(w,key);if(!isfinite(n)){w->failed=true;return;}char text[32];int length=phw_json_format_float32(text,sizeof(text),n);if(length<0)w->failed=true;else phw_bytes(w,text,(size_t)length);}
void phw_bool(ph_writer *w,const char *key,bool yes){phw_prefix(w,key);if(w->format){unsigned char c=(w->format==PH_WIRE_CBOR?0xf4:0xc2)+yes;phw_bytes(w,&c,1);}else phw_bytes(w,yes?"true":"false",yes?4:5);}
void phw_null(ph_writer *w,const char *key){phw_prefix(w,key);if(w->format){unsigned char c=w->format==PH_WIRE_CBOR?0xf6:0xc0;phw_bytes(w,&c,1);}else phw_bytes(w,"null",4);}
/* Iterative conversion keeps the call stack independent of document depth. */
static void phw_visit(ph_writer *w,const char *key,const ph_value *v,bool canonical){
 ph_iterator parents[PHW_DEPTH];unsigned used=0;ph_value current;char name[128];
 if(!v){w->failed=true;return;}current=*v;
 for(;;){
  if(current.kind==PHV_MISSING){w->failed=true;return;}
  if(!canonical&&current.format==w->format){phw_prefix(w,key);phw_bytes(w,current.begin,(size_t)(current.end-current.begin));}
  else switch(current.kind){
   case PHV_UINT:phw_uint(w,key,current.count);break;
   case PHV_NEG:phw_int(w,key,-1-(int64_t)current.count);break;
   case PHV_REAL:phw_number(w,key,current.real);break;
   case PHV_FALSE:case PHV_TRUE:phw_bool(w,key,current.kind==PHV_TRUE);break;
   case PHV_NULL:phw_null(w,key);break;
   case PHV_BYTES:phw_bytes_value(w,key,current.text,current.size);break;
   case PHV_TEXT:
    if(current.format)phw_text_n(w,key,(const char *)current.text,current.size);
    else {size_t n;if(!phv_unquote(current.text,current.text+current.size,NULL,0,&n)){w->failed=true;return;}
     char *text=json_heap_malloc(n+1);if(!text){w->failed=true;return;}
     if(!phv_text(&current,text,n+1))w->failed=true;else phw_text_n(w,key,text,n);json_heap_free(text);}
    break;
   case PHV_ARRAY:case PHV_MAP:
    phw_begin(w,key,current.kind==PHV_ARRAY);
    if(w->failed||used>=PHW_DEPTH||!phv_iter(&current,&parents[used])){w->failed=true;return;}
    used++;break;
   default:w->failed=true;return;
  }
  while(used&&!w->failed){
   ph_iterator *it=&parents[used-1];
   if(it->index<it->count){if(!phv_next(it,name,&current)){w->failed=true;return;}key=it->kind==PHV_ARRAY?NULL:name;break;}
   phw_end(w);used--;
  }
  if(!used||w->failed)return;
 }
}
void phw_value(ph_writer *w,const char *key,const ph_value *v){phw_visit(w,key,v,false);}
void phw_value_canonical(ph_writer *w,const char *key,const ph_value *v){phw_visit(w,key,v,true);}
void phw_discard(ph_writer *w){if(w->wire&&!w->fixed)printbuf_free(w->wire);w->wire=NULL;w->failed=true;}
struct printbuf *phw_take(ph_writer *w){if(w->fixed||w->failed||w->depth||w->root_key||!w->wire||!w->wire->bpos){phw_discard(w);return NULL;}struct printbuf *wire=w->wire;w->wire=NULL;return wire;}

bool phv_read_action(ph_wire_format format,const void *bytes,size_t size,status_request_t *request,const char *action){
 ph_value root,value,params;memset(request,0,sizeof(*request));
 if(!phv_parse(format,bytes,size,&root)||root.kind!=PHV_MAP||!phv_keys(&root,"|action||params||request_id|")||
    !phv_get(&root,"action",&value)||!phv_equal_text(&value,action)||!phv_get(&root,"params",&params)||params.kind!=PHV_MAP||!phv_keys(&params,"|view|"))return false;
 if(strcmp(action,"status.get")&&params.count)return false;
 if(phv_get(&params,"view",&value)){if(phv_equal_text(&value,"live"))request->live=true;else if(!phv_equal_text(&value,"full"))return false;}
 if(phv_get(&root,"request_id",&value)){if(!phv_text(&value,request->id,sizeof(request->id)))return false;for(const char *p=request->id;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='.'||*p=='-'))return false;if(!*request->id)return false;request->has_id=true;}
 return true;
}

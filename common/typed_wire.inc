/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "typed_wire.h"
#include "binary_reader.h"
#include <string.h>
#include <math.h>

typedef struct { const unsigned char *p,*end; ph_wire_format format; unsigned nodes; } phv_reader;
typedef struct { const unsigned char *children; uint32_t bloom[2]; uint16_t index,limit; unsigned kind; } phv_frame;
static bool phv_scan(phv_reader *,ph_value *,unsigned,bool);
static bool phv_head(phv_reader *r,ph_value *v)
{
 memset(v,0,sizeof(*v));v->kind=PHV_MISSING;v->format=r->format;
 binary_reader b={.p=r->p,.end=r->end,.f=r->format,.nodes=r->nodes};binary_item item={0};
 if(!binary_token(&b,&item))return false;
 v->begin=r->p;v->end=b.p;v->kind=item.kind;v->text=item.text;
 if(item.kind==PHV_REAL)v->real=item.real;else v->count=item.n;
 if(item.kind==PHV_TEXT||item.kind==PHV_BYTES)v->size=(size_t)item.n;
 r->p=b.p;r->nodes=b.nodes;return true;
}
/* Revisits an already validated immutable prefix without duplicating frames. */
static bool phv_skip_validated(phv_reader *r,ph_value *v)
{
 if(!phv_head(r,v))return false;
 unsigned pending=v->kind==PHV_MAP?(unsigned)v->count*2:v->kind==PHV_ARRAY?(unsigned)v->count:0;
 while(pending){ph_value item;if(!phv_head(r,&item))return false;pending--;if(item.kind==PHV_MAP)pending+=(unsigned)item.count*2;else if(item.kind==PHV_ARRAY)pending+=(unsigned)item.count;}
 v->end=r->p;return true;
}
static uint32_t phv_key_hash(const char *key)
{ uint32_t h=UINT32_C(1939669891);while(*key){h^=(unsigned char)*key++;h*=UINT32_C(435);}return h; }
static bool phv_duplicate(phv_reader *r,const phv_frame *frame,const char *name,unsigned *budget)
{
 phv_reader previous={frame->children,r->p,r->format,0};
 for(unsigned j=0;j<frame->index;j++){
  if(!*budget)return true;--*budget;ph_value key,value;
  if(!phv_skip_validated(&previous,&key)||phv_equal_text(&key,name))return true;
  if(!phv_skip_validated(&previous,&value))return true;
 }
 return false;
}
static bool phv_scan(phv_reader *r,ph_value *v,unsigned depth,bool validate)
{
 phv_frame stack[PH_BINARY_DEPTH];unsigned used=0,budget=PH_BINARY_NODES*8;ph_value item;
 if(depth>=PH_BINARY_DEPTH||!phv_head(r,v))return false;
 if(v->kind!=PHV_ARRAY&&v->kind!=PHV_MAP)return true;
 stack[used++]=(phv_frame){.children=r->p,.limit=(uint16_t)v->count,.kind=v->kind};
 while(used){
  phv_frame *f=&stack[used-1];
  if(f->index==f->limit){used--;continue;}
  if(f->kind==PHV_MAP){
   if(!phv_head(r,&item)||item.kind!=PHV_TEXT)return false;
   char key[128];if(!phv_text(&item,key,sizeof(key)))return false;
   if(validate){unsigned hash=phv_key_hash(key)&63,word=hash>>5;uint32_t bit=UINT32_C(1)<<(hash&31);
    if((f->bloom[word]&bit)&&phv_duplicate(r,f,key,&budget))return false;f->bloom[word]|=bit;}
  }
  if(!phv_head(r,&item))return false;f->index++;
  if(item.kind==PHV_ARRAY||item.kind==PHV_MAP){
   if(depth+used>=PH_BINARY_DEPTH)return false;
   stack[used++]=(phv_frame){.children=r->p,.limit=(uint16_t)item.count,.kind=item.kind};
  }
 }
 v->end=r->p;return true;
}
bool phv_parse(ph_wire_format format,const void *bytes,size_t size,ph_value *out)
{
 if(!out)return false;memset(out,0,sizeof(*out));out->kind=PHV_MISSING;
 if(!bytes||!size||size>PH_BINARY_BYTES||format!=PH_WIRE_MSGPACK)return false;
 phv_reader r={bytes,(const unsigned char *)bytes+size,format,0};
 return phv_scan(&r,out,0,true)&&r.p==r.end;
}
bool phv_bytes(const ph_value *v,const void **bytes,size_t *size)
{ if(!v||v->kind!=PHV_BYTES||!bytes||!size)return false;*bytes=v->text;*size=v->size;return true; }
bool phv_text(const ph_value *v,char *text,size_t capacity)
{ if(!v||v->kind!=PHV_TEXT||!text||v->size>=capacity)return false;memcpy(text,v->text,v->size);text[v->size]=0;return true; }
bool phv_equal_text(const ph_value *v,const char *text)
{ return v&&v->kind==PHV_TEXT&&text&&strlen(text)==v->size&&!memcmp(v->text,text,v->size); }
static bool phv_child_reader(const ph_value *v,phv_reader *r)
{
 if(!v||(v->kind!=PHV_MAP&&v->kind!=PHV_ARRAY))return false;
 *r=(phv_reader){v->begin,v->end,v->format,0};ph_value head;return phv_head(r,&head);
}
bool phv_iter(const ph_value *v,ph_iterator *it)
{
 phv_reader r;if(!it||!phv_child_reader(v,&r))return false;
 *it=(ph_iterator){r.p,r.end,v->format,v->kind,0,(unsigned)v->count};return true;
}
bool phv_next(ph_iterator *it,char key[128],ph_value *out)
{
 if(!it||!out||it->index>=it->count)return false;
 phv_reader r={it->cursor,it->end,it->format,0};
 if(it->kind==PHV_MAP){ph_value k;if(!phv_scan(&r,&k,0,false)||k.kind!=PHV_TEXT)return false;if(key&&!phv_text(&k,key,128))return false;}
 if(!phv_scan(&r,out,0,false))return false;it->cursor=r.p;it->index++;return true;
}
bool phv_pair(const ph_value *v,unsigned index,char key[128],ph_value *out)
{
 if(out){memset(out,0,sizeof(*out));out->kind=PHV_MISSING;}ph_iterator it;
 if(!out||!v||v->kind!=PHV_MAP||index>=v->count||!phv_iter(v,&it))return false;
 for(unsigned i=0;i<=index;i++)if(!phv_next(&it,i==index?key:NULL,out))return false;return true;
}
bool phv_get(const ph_value *v,const char *name,ph_value *out)
{
 if(!out)return false;memset(out,0,sizeof(*out));out->kind=PHV_MISSING;ph_iterator it;
 if(!v||!name||v->kind!=PHV_MAP||!phv_iter(v,&it))return false;
 while(it.index<it.count){char key[128];ph_value value;if(!phv_next(&it,key,&value))return false;if(!strcmp(key,name)){*out=value;return true;}}return false;
}
bool phv_at(const ph_value *v,unsigned index,ph_value *out)
{
 if(!out)return false;memset(out,0,sizeof(*out));out->kind=PHV_MISSING;ph_iterator it;
 if(!v||v->kind!=PHV_ARRAY||index>=v->count||!phv_iter(v,&it))return false;
 for(unsigned i=0;i<=index;i++)if(!phv_next(&it,NULL,out))return false;return true;
}
bool phv_int(const ph_value *v,int64_t *out)
{ if(!v||!out||(v->kind!=PHV_UINT&&v->kind!=PHV_NEG))return false;*out=v->kind==PHV_NEG?-1-(int64_t)v->count:(int64_t)v->count;return true; }
bool phv_number(const ph_value *v,double *out)
{ int64_t n;if(!v||!out)return false;if(v->kind==PHV_REAL){*out=v->real;return true;}if(!phv_int(v,&n))return false;*out=(double)n;return true; }
bool phv_bool(const ph_value *v,bool *out)
{ if(!v||!out||(v->kind!=PHV_FALSE&&v->kind!=PHV_TRUE))return false;*out=v->kind==PHV_TRUE;return true; }
bool phv_keys(const ph_value *v,const char *allowed)
{
 ph_iterator it;if(!v||!allowed||v->kind!=PHV_MAP||!phv_iter(v,&it))return false;
 while(it.index<it.count){char key[128],needle[130];ph_value item;if(!phv_next(&it,key,&item))return false;
  size_t n=strlen(key);if(!n||strchr(key,'|'))return false;needle[0]='|';memcpy(needle+1,key,n);needle[n+1]='|';needle[n+2]=0;if(!strstr(allowed,needle))return false;}
 return true;
}

static void phw_bytes(ph_writer *w,const void *data,size_t n)
{
 if(w->failed)return;if(!w->wire||w->wire->len>PHW_MAX_BYTES||n>PHW_MAX_BYTES-w->wire->len){w->failed=true;return;}
 size_t need=w->wire->len+n+1;
 if(need>w->wire->cap){
  if(w->fixed){w->failed=true;return;}size_t cap=w->wire->cap;
  while(cap<need){size_t next=cap+cap/2+64;cap=next>PHW_MAX_BYTES+1?PHW_MAX_BYTES+1:next;}
  if(!ph_buf_reserve(w->wire,cap)){w->failed=true;return;}
 }
 if(w->wire->buf&&n)memcpy(w->wire->buf+w->wire->len,data,n);
 w->wire->len+=n;if(w->wire->buf)w->wire->buf[w->wire->len]=0;
}
bool phw_init_capacity(ph_writer *w,ph_wire_format format,size_t capacity)
{
 memset(w,0,sizeof(*w));w->format=format;
 if(format!=PH_WIRE_MSGPACK||!capacity||capacity>PHW_MAX_BYTES+1){w->failed=true;return false;}
 w->wire=ph_buf_new(capacity);if(!w->wire){w->failed=true;return false;}return true;
}
bool phw_init(ph_writer *w,ph_wire_format format){return phw_init_capacity(w,format,128);}
bool phw_init_measure(ph_writer *w,ph_wire_format format,ph_buf *wire)
{
 memset(w,0,sizeof(*w));w->fixed=true;w->format=format;
 if(!wire||format!=PH_WIRE_MSGPACK){w->failed=true;return false;}
 *wire=(ph_buf){.cap=PHW_MAX_BYTES+1};w->wire=wire;return true;
}
bool phw_init_fixed(ph_writer *w,ph_wire_format format,ph_buf *wire,void *buffer,size_t size)
{
 memset(w,0,sizeof(*w));w->fixed=true;w->format=format;
 if(!wire||!buffer||!size||size>PHW_MAX_BYTES+1||format!=PH_WIRE_MSGPACK){w->failed=true;return false;}
 *wire=(ph_buf){.buf=buffer,.cap=size};wire->buf[0]=0;w->wire=wire;return true;
}
size_t phw_finish_fixed(ph_writer *w){return w->fixed&&!w->failed&&!w->depth&&!w->root_key&&w->wire?w->wire->len:0;}
static void phw_head(ph_writer *w,unsigned kind,uint64_t n)
{ unsigned char bytes[9];size_t count=ph_wire_head(w->format,kind,n,bytes);if(!count)w->failed=true;else phw_bytes(w,bytes,count); }
static void phw_quote(ph_writer *w,const char *text,size_t size)
{ if(!text||size>PHW_MAX_BYTES||!ph_wire_utf8((const unsigned char *)text,size)){w->failed=true;return;}phw_head(w,PHV_TEXT,size);phw_bytes(w,text,size); }
void phw_root_key(ph_writer *w,const char *key)
{ if(w->root_key||!key||!w->depth||w->arrays[w->depth-1])w->failed=true;else w->root_key=key; }
static void phw_prefix(ph_writer *w,const char *key)
{
 if(w->failed)return;if(w->root_key){if(key){w->failed=true;return;}key=w->root_key;w->root_key=NULL;}
 if(!w->depth){if(key||!w->wire||w->wire->len)w->failed=true;return;}
 unsigned d=w->depth-1;if(w->counts[d]==UINT16_MAX){w->failed=true;return;}w->counts[d]++;
 if(!w->arrays[d]){if(!key||strlen(key)>=128){w->failed=true;return;}phw_quote(w,key,strlen(key));}else if(key)w->failed=true;
}
void phw_begin(ph_writer *w,const char *key,bool array)
{
 phw_prefix(w,key);if(w->failed)return;if(w->depth>=PHW_DEPTH){w->failed=true;return;}
 unsigned d=w->depth++;w->arrays[d]=array;w->counts[d]=0;w->starts[d]=w->wire->len;
 unsigned char b[3]={array?0xdc:0xde,0,0};phw_bytes(w,b,3);
}
void phw_end(ph_writer *w)
{
 if(w->failed)return;if(!w->depth){w->failed=true;return;}unsigned d=--w->depth;
 if(w->wire->buf){size_t p=w->starts[d];w->wire->buf[p+1]=(char)(w->counts[d]>>8);w->wire->buf[p+2]=(char)w->counts[d];}
}
void phw_bytes_value(ph_writer *w,const char *key,const void *bytes,size_t size)
{ phw_prefix(w,key);if(!bytes&&size){w->failed=true;return;}phw_head(w,PHV_BYTES,size);phw_bytes(w,bytes,size); }
void phw_text_n(ph_writer *w,const char *key,const char *text,size_t size)
{ phw_prefix(w,key);phw_quote(w,text,size); }
void phw_text(ph_writer *w,const char *key,const char *text)
{ if(!text){w->failed=true;return;}phw_text_n(w,key,text,strlen(text)); }
void phw_uint(ph_writer *w,const char *key,uint64_t n)
{ phw_prefix(w,key);if(n>PH_BINARY_SAFE){w->failed=true;return;}phw_head(w,0,n); }
void phw_int(ph_writer *w,const char *key,int64_t n)
{
 if(n>=0){phw_uint(w,key,(uint64_t)n);return;}phw_prefix(w,key);uint64_t magnitude=(uint64_t)(-(n+1));
 if(magnitude>=PH_BINARY_SAFE){w->failed=true;return;}phw_head(w,1,magnitude);
}
void phw_i32(ph_writer *w,const char *key,int32_t n){phw_int(w,key,n);}
void phw_u32(ph_writer *w,const char *key,uint32_t n){phw_uint(w,key,n);}
void phw_number(ph_writer *w,const char *key,double n)
{ phw_prefix(w,key);if(!isfinite(n)){w->failed=true;return;}unsigned char b[9];phw_bytes(w,b,ph_wire_double(w->format,n,b)); }
void phw_bool(ph_writer *w,const char *key,bool yes)
{ phw_prefix(w,key);unsigned char c=0xc2+yes;phw_bytes(w,&c,1); }
void phw_null(ph_writer *w,const char *key)
{ phw_prefix(w,key);unsigned char c=0xc0;phw_bytes(w,&c,1); }
/* Iterative re-emission preserves map iteration order; it does not sort keys. */
static void phw_visit(ph_writer *w,const char *key,const ph_value *v,bool canonical)
{
 ph_iterator parents[PHW_DEPTH];unsigned used=0;ph_value current;char name[128];
 if(!v){w->failed=true;return;}current=*v;
 for(;;){
  if(current.kind==PHV_MISSING){w->failed=true;return;}
  if(!canonical&&current.format==w->format&&current.kind!=PHV_ARRAY&&current.kind!=PHV_MAP){phw_prefix(w,key);phw_bytes(w,current.begin,(size_t)(current.end-current.begin));}
  else switch(current.kind){
   case PHV_UINT:phw_uint(w,key,current.count);break;
   case PHV_NEG:phw_int(w,key,-1-(int64_t)current.count);break;
   case PHV_REAL:phw_number(w,key,current.real);break;
   case PHV_FALSE:case PHV_TRUE:phw_bool(w,key,current.kind==PHV_TRUE);break;
   case PHV_NULL:phw_null(w,key);break;
   case PHV_TEXT:phw_text_n(w,key,(const char *)current.text,current.size);break;
   case PHV_BYTES:phw_bytes_value(w,key,current.text,current.size);break;
   case PHV_ARRAY:case PHV_MAP:
    phw_begin(w,key,current.kind==PHV_ARRAY);
    if(w->failed||used>=PHW_DEPTH||!phv_iter(&current,&parents[used])){w->failed=true;return;}
    used++;break;
   default:w->failed=true;return;
  }
  while(used&&!w->failed){ph_iterator *it=&parents[used-1];
   if(it->index<it->count){if(!phv_next(it,name,&current)){w->failed=true;return;}key=it->kind==PHV_ARRAY?NULL:name;break;}
   phw_end(w);used--;
  }
  if(!used||w->failed)return;
 }
}
void phw_cached_value(ph_writer *w,const char *key,const ph_buf *value,unsigned depth)
{
 if(w->failed)return;
 if(!value||!value->buf||!value->len||value->len>=value->cap||
    w->depth>PHW_DEPTH||depth>PHW_DEPTH-w->depth||value==w->wire||
    (w->wire&&value->buf==w->wire->buf)){w->failed=true;return;}
 phw_prefix(w,key);phw_bytes(w,value->buf,value->len);
}
void phw_value(ph_writer *w,const char *key,const ph_value *v){phw_visit(w,key,v,false);}
void phw_value_canonical(ph_writer *w,const char *key,const ph_value *v){phw_visit(w,key,v,true);}
void phw_discard(ph_writer *w){if(w->wire&&!w->fixed)ph_buf_free(w->wire);w->wire=NULL;w->failed=true;}
ph_buf *phw_take(ph_writer *w)
{
 if(w->fixed||w->failed||w->depth||w->root_key||!w->wire||!w->wire->len){phw_discard(w);return NULL;}
 ph_buf *wire=w->wire;w->wire=NULL;return wire;
}

#include "widgets.h"
#include <math.h>
#include <string.h>
typedef struct {const uint8_t *p,*end;} reader;
static bool byte(reader *r,uint8_t *v){if(r->p==r->end)return false;*v=*r->p++;return true;}
static bool number(reader *r,uint32_t *n){
 uint8_t b;if(!byte(r,&b))return false;if(b<128){*n=b;return true;}
 unsigned size=b==0xcc?1:b==0xcd?2:b==0xce?4:0;if(!size||(size_t)(r->end-r->p)<size)return false;
 *n=0;while(size--)*n=(*n<<8)|*r->p++;return true;
}
static bool array(reader *r,uint32_t *n){uint8_t b;if(!byte(r,&b)||b<0x90||b>0x9f)return false;*n=b&15;return true;}
/* Validate complete UTF-8 scalar sequences; reject controls, overlong, surrogate
 * and out-of-range codepoints. This also prevents embedded NUL truncation. */
static bool utf8(const uint8_t *p,size_t n){
 for(size_t i=0;i<n;){uint32_t c=p[i++],min;unsigned k;
  if(c<0x80){if(c<32||c==127)return false;continue;}
  if(c>=0xc2&&c<=0xdf){c&=31;k=1;min=128;}else if(c>=0xe0&&c<=0xef){c&=15;k=2;min=2048;}else if(c>=0xf0&&c<=0xf4){c&=7;k=3;min=65536;}else return false;
  if(n-i<k)return false;
  while(k--){unsigned b=p[i++];if((b&0xc0)!=0x80)return false;c=(c<<6)|(b&63);}if(c<min||c>0x10ffff||(c>=0xd800&&c<=0xdfff))return false;
 }return true;
}
static bool string(reader *r,char *dst,size_t capacity){
 uint8_t b;size_t n;if(!byte(r,&b))return false;if((b&0xe0)==0xa0)n=b&31;else if(b==0xd9){if(!byte(r,&b))return false;n=b;}else return false;
 if(n>=capacity||(size_t)(r->end-r->p)<n||!utf8(r->p,n))return false;
 memcpy(dst,r->p,n);dst[n]=0;r->p+=n;return true;
}
static bool real(reader *r,float *v){
 if(r->p==r->end)return false;
 if(*r->p!=0xca){uint32_t n;if(!number(r,&n))return false;*v=(float)n;}else{
  r->p++;if(r->end-r->p<4)return false;uint32_t n=0;for(unsigned i=0;i<4;i++)n=(n<<8)|*r->p++;memcpy(v,&n,4);
 }return isfinite(*v)&&fabsf(*v)<=1000000.f;
}
bool screen_widget_valid(const screen_widget *w,uint32_t now){return w->valid&&!w->expired&&(uint32_t)(now-w->at)<w->ttl_ms;}
bool screen_widget_apply(screen_widgets *s,const void *data,size_t size,uint32_t now){
 if(!s||!data||!size||size>768)return false;
 reader r={data,(const uint8_t *)data+size};screen_widget w={0};uint32_t n,version,slot,type,source,ttl;
 if(!array(&r,&n)||n!=9||!number(&r,&version)||version!=1||!number(&r,&slot)||slot>=SCREEN_WIDGETS||!number(&r,&type)||type>4||!number(&r,&source)||source>2||!number(&r,&ttl)||ttl<1||ttl>86400||!string(&r,w.title,sizeof(w.title))||!w.title[0]||!string(&r,w.unit,sizeof(w.unit))||!string(&r,w.text,sizeof(w.text)))return false;
 /* values may use fixarray or array16 for the full sixteen-point bound. */
 if(r.p<r.end&&*r.p==0xdc){r.p++;if(r.end-r.p<2)return false;n=(uint32_t)r.p[0]*256+r.p[1];r.p+=2;}else if(!array(&r,&n))return false;
 if(n>SCREEN_WIDGET_POINTS||(type==4&&n>4)||(type==0&&n!=1)||(type==1&&n!=0)||(type>=2&&n<2))return false;
 float sum=0;for(unsigned i=0;i<n;i++){if(!real(&r,&w.values[i])||(type==4&&w.values[i]<0))return false;sum+=w.values[i];}
 if(r.p!=r.end||(type==4&&sum<=0))return false;
 w.valid=true;w.type=(uint8_t)type;w.source=(uint8_t)source;w.count=(uint8_t)n;w.at=now;w.ttl_ms=ttl*1000;s->items[slot]=w;return true;
}

#include "screen_link.h"
#include <stdio.h>
#include <string.h>
static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
void screen_link_init(screen_link *s,screen_link_receive cb,void *ctx){*s=(screen_link){.receive=cb,.context=ctx};}
void screen_link_feed(screen_link *s,const void *data,size_t n){
 const unsigned char *p=data;
 while(n--){unsigned char c=*p++;if(c=='\n'){
  if(!s->overflow&&s->used&&s->line[s->used-1]=='\r'){s->line[--s->used]=0;if(s->receive)s->receive(s->context,s->line,s->used);}
  s->used=0;s->overflow=false;
 }else if(!s->overflow){if(s->used+1<sizeof(s->line))s->line[s->used++]=c;else{s->overflow=true;s->used=0;}}}
}
uint32_t screen_link_crc32(const void *data,size_t n){uint32_t c=~0u;const unsigned char *p=data;while(n--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^((c&1)?0xedb88320u:0);}return ~c;}
size_t screen_link_encode(char *out,size_t cap,uint32_t session,uint32_t seq,const void *data,size_t n){
 if(!out||!cap||!session||!seq||!data||!n||n>SCREEN_LINK_CHUNK)return 0;
 int k=snprintf(out,cap,"screen_ext 1 %08x %08x %08x ",(unsigned)session,(unsigned)seq,(unsigned)screen_link_crc32(data,n));
 size_t encoded=(n+2)/3*4;if(k<0||(size_t)k+encoded+3>cap)return 0;
 const unsigned char *p=data;size_t at=(size_t)k;
 for(size_t i=0;i<n;i+=3){uint32_t v=(uint32_t)p[i]<<16;if(i+1<n)v|=(uint32_t)p[i+1]<<8;if(i+2<n)v|=p[i+2];out[at++]=alphabet[v>>18];out[at++]=alphabet[v>>12&63];out[at++]=i+1<n?alphabet[v>>6&63]:'=';out[at++]=i+2<n?alphabet[v&63]:'=';}
 out[at++]='\r';out[at++]='\n';out[at]=0;return at;
}
static int digit(char c){const char *p=c?strchr(alphabet,c):NULL;return p?(int)(p-alphabet):-1;}
static bool hex8(const char *p,uint32_t *v){*v=0;for(unsigned i=0;i<8;i++){char c=p[i];unsigned d=c>='0'&&c<='9'?(unsigned)(c-'0'):c>='a'&&c<='f'?(unsigned)(c-'a'+10):16;if(d>15)return false;*v=(*v<<4)|d;}return true;}
bool screen_link_decode(const char *p,size_t n,uint32_t *session,uint32_t *seq,void *data,size_t *length){
 if(!p||!session||!seq||!data||!length||n<44||memcmp(p,"screen_ext 1 ",13)||p[21]!=' '||p[30]!=' '||p[39]!=' ')return false;
 uint32_t sid,serial,crc;if(!hex8(p+13,&sid)||!hex8(p+22,&serial)||!hex8(p+31,&crc)||!sid||!serial)return false;
 size_t count=n-40;if(!count||count%4||count>1024)return false;unsigned char *out=data;size_t used=0;
 for(size_t i=40;i<n;i+=4){int a=digit(p[i]),b=digit(p[i+1]),c=digit(p[i+2]),d=digit(p[i+3]);bool pad2=p[i+2]=='=',pad3=p[i+3]=='=';
  if(a<0||b<0||(!pad2&&c<0)||(!pad3&&d<0)||(pad2&&!pad3)||((pad2||pad3)&&i+4!=n)||(pad2&&(b&15))||(pad3&&!pad2&&(c&3)))return false;
  unsigned bytes=pad2?1:pad3?2:3;if(used+bytes>*length)return false;
  uint32_t v=(unsigned)a<<18|(unsigned)b<<12|(unsigned)(c<0?0:c)<<6|(unsigned)(d<0?0:d);
  out[used++]=v>>16;if(bytes>1)out[used++]=v>>8;if(bytes>2)out[used++]=v;
 }
 if(screen_link_crc32(out,used)!=crc)return false;
 *session=sid;*seq=serial;*length=used;return true;
}

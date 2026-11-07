#include "screen_caps.h"
#include <stdio.h>
#include <string.h>
static bool hex(const char *p,uint32_t *v){
 *v=0;for(unsigned i=0;i<8;i++){unsigned c=(unsigned char)p[i],x;if(c>='0'&&c<='9')x=c-'0';else if(c>='a'&&c<='f')x=c-'a'+10;else return false;*v=(*v<<4)|x;}return true;
}
static bool parse(const char *p,size_t n,const char *prefix,uint32_t *s,uint32_t *c){
 size_t k=strlen(prefix);if(n!=k+17||memcmp(p,prefix,k)||p[k+8]!=' ')return false;
 return hex(p+k,s)&&*s&&hex(p+k+9,c)&&!(*c&~SCREEN_CAP_KNOWN);
}
void screen_caps_reset(screen_caps *s){*s=(screen_caps){0};}
static size_t write_line(char *p,size_t n,const char *prefix,uint32_t s,uint32_t c){
 if(!p||!s||c&~SCREEN_CAP_KNOWN)return 0;
 int used=snprintf(p,n,"%s%08lx %08lx\r\n",prefix,(unsigned long)s,(unsigned long)c);
 return used>0&&(size_t)used<n?(size_t)used:0;
}
size_t screen_caps_offer(screen_caps *s,char *p,size_t n,uint32_t session,uint32_t caps){
 screen_caps_reset(s);size_t used=write_line(p,n,"screen_caps 1 ",session,caps);
 if(used){s->session=session;s->capabilities=caps;}return used;
}
bool screen_caps_parse_offer(const char *p,size_t n,uint32_t *s,uint32_t *c){return parse(p,n,"screen_caps 1 ",s,c);}
size_t screen_caps_ack(char *p,size_t n,uint32_t s,uint32_t c){return write_line(p,n,"+screen_caps:1 ",s,c);}
bool screen_caps_accept(screen_caps *s,const char *p,size_t n){
 uint32_t session,caps;if(!parse(p,n,"+screen_caps:1 ",&session,&caps)||session!=s->session||(caps&~s->capabilities))return false;
 s->capabilities=caps;s->ready=true;return true;
}

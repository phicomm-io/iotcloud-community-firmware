#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned allocations, fail_at, live, adds, updates;
static void *allocate(size_t n){if(++allocations==fail_at)return NULL;void*p=malloc(n);if(p)live++;return p;}
static void release(void*p){if(p){assert(live);live--;free(p);}}
static char *duplicate(const char*s){size_t n=strlen(s)+1;char*p=allocate(n);if(p)memcpy(p,s,n);return p;}
/* Existing SDK StringUtils escaping oracle, extracted separately by Python. */
#define malloc allocate
#define free release
#include "legacy_escape.inc"
#undef malloc
#undef free
/* Emulate the intended old overlapping sprintf behavior using a disjoint
 * scratch. All format strings and field ordering come from the real old SDK. */
static int legacy_sprintf(char *out,const char *fmt,...){char tmp[8192];va_list ap;va_start(ap,fmt);int n=vsnprintf(tmp,sizeof(tmp),fmt,ap);va_end(ap);assert(n>=0&&(size_t)n<sizeof(tmp));memcpy(out,tmp,(size_t)n+1);return n;}
#define sprintf legacy_sprintf
#define free release
#define _bonjour_generate_txt_record legacy_record
#include "legacy_builder.inc"
#undef _bonjour_generate_txt_record
#undef free
#undef sprintf
#define malloc allocate
#define free release
#include "production.inc"
#undef malloc
#undef free
static void mdns_add_record(mdns_init_t x,WiFi_Interface i,int ttl){assert(x.txt_record&&strlen(x.txt_record)<500);assert(i==Station||i==Soft_AP);assert(ttl==(i==Soft_AP?1500:10));adds++;}
static void mdns_update_txt_record(const char*s,WiFi_Interface i,char*t){assert(!strcmp(s,"_easylink_config._tcp.local."));assert(i==Station||i==Soft_AP);assert(strlen(t)<500);updates++;}
int main(void){
 system_context_t ctx={0};strcpy(ctx.micoStatus.mac,"AA:BB:CC:DD:EE:FF");strcpy(ctx.micoStatus.rf_version,"rf.1..2");
 const uint32_t ids[]={0,1,0x7fffffff,0x80000000,0xfffffffe,0xffffffff};
 for(unsigned mode=0;mode<2;mode++)for(unsigned id=0;id<6;id++)for(unsigned dots=0;dots<3;dots++){
  strcpy(fw,dots==0?"1.2..3":dots==1?"":"/.a/b.");strcpy(hardware,"rev..1");strcpy(model,"TC1.A1");strcpy(manufacturer,"MX.CHIP");
  char legacy[8192],actual[500];legacy_record(legacy,ids[id],mode,&ctx);assert(!live);
  assert(_bonjour_generate_txt_record(actual,sizeof(actual),ids[id],mode,&ctx)==kNoErr);assert(!strcmp(actual,legacy));
  size_t wanted=strlen(legacy)+1;
  for(size_t cap=0;cap<=wanted+1;cap++){
   unsigned char guard[520];memset(guard,0xa5,sizeof(guard));int rc=_bonjour_generate_txt_record((char*)guard+1,cap,ids[id],mode,&ctx);
   assert(guard[0]==0xa5&&guard[cap+1]==0xa5);assert(rc==(cap<wanted?kSizeErr:kNoErr));if(!rc)assert(!strcmp((char*)guard+1,legacy));
  }
 }
 strcpy(fw,"1");strcpy(hardware,"1");model[0]=manufacturer[0]=0;ctx.micoStatus.rf_version[0]=0;
 char tmp[500];assert(!_bonjour_generate_txt_record(tmp,sizeof(tmp),1,Station,&ctx));size_t base=strlen(tmp);
 memset(model,'x',499-base);model[499-base]=0;assert(!_bonjour_generate_txt_record(tmp,sizeof(tmp),1,Station,&ctx)&&strlen(tmp)==499);
 model[499-base]='x';model[500-base]=0;assert(_bonjour_generate_txt_record(tmp,sizeof(tmp),1,Station,&ctx)==kSizeErr);
 unsigned before=adds;assert(easylink_bonjour_start(Station,1,&ctx)==kSizeErr&&!live&&adds==before);
 before=updates;assert(easylink_bonjour_update(Station,1,&ctx)==kSizeErr&&!live&&updates==before);
 model[0]=0;assert(!easylink_bonjour_start(Soft_AP,1,&ctx)&&!live&&adds==1);
 assert(!easylink_bonjour_update(Station,1,&ctx)&&!live&&updates==1);
 fail_at=allocations+1;assert(easylink_bonjour_start(Station,1,&ctx)==kNoMemoryErr&&!live&&adds==1);
 fail_at=allocations+1;assert(easylink_bonjour_update(Station,1,&ctx)==kNoMemoryErr&&!live&&updates==1);
 puts("actual legacy/new TXT bytes, escaping, uint32 IDs, exact500 boundary, canaries, no publish on size/OOM, cleanup PASS");
}

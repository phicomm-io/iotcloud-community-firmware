#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef int OSStatus,WiFi_Interface;
enum {kNoErr=0,kParamErr=-1,kNoMemoryErr=-2,kNoResourcesErr=-3,Station=0,Soft_AP=1,RECORD_REMOVED=0,RECORD_UPDATE=1,RECORD_SUSPEND=2,MICO_APPLICATION_PRIORITY=7};
typedef struct {char *service_name,*host_name,*instance_name,*txt_record;unsigned service_port;} mdns_init_t;
typedef struct {char *hostname,*instance_name,*service_name,*txt_att;char instance_name_suffix[4];WiFi_Interface interface;uint32_t ttl;uint16_t port;uint8_t count_down,count_down_max;unsigned state;} dns_sd_service_record_t;
static dns_sd_service_record_t available_services[4];static uint8_t available_service_count=4;
static bool bonjour_instance=true,thread_fail;static int bonjour_mutex;static void *_bonjour_announce_handler;
static int allowance=-1;static unsigned live;
static void *allocate(size_t n){if(!allowance)return NULL;if(allowance>0)allowance--;void*p=malloc(n);if(p)live++;return p;}
static void release(void*p){if(p){assert(live);live--;free(p);}}
static char *duplicate(const char*s){size_t n=strlen(s)+1;char*p=allocate(n);if(p)memcpy(p,s,n);return p;}
static int find_record_by_service(const char*s,int iface){for(unsigned i=0;i<4;i++)if(available_services[i].service_name&&available_services[i].interface==iface&&!strcmp(available_services[i].service_name,s))return i;return 255;}
static int find_empty_record(void){for(unsigned i=0;i<4;i++)if(!available_services[i].service_name)return i;return 255;}
static bool is_service_match(dns_sd_service_record_t*r,char*s,int iface){return r->service_name&&r->interface==iface&&!strcmp(r->service_name,s);}
static int start_bonjour_service(void){bonjour_instance=true;return 0;}
static void _bonjour_send_anounce_thread(void*p){(void)p;}
static int mico_rtos_create_thread(void **p,int priority,const char*n,void(*run)(void*),unsigned stack,int arg){(void)priority;(void)n;(void)run;(void)stack;(void)arg;if(thread_fail)return -9;*p=(void*)1;return 0;}
static void mico_rtos_lock_mutex(int*p){assert(!*p);*p=1;}
static void mico_rtos_unlock_mutex(int*p){assert(*p);*p=0;}
#define require_action(C,L,A) do{if(!(C)){A;goto L;}}while(0)
#define require_noerr(C,L) do{if(C)goto L;}while(0)
#define malloc allocate
#define free release
#define __strdup duplicate
#include "mdns_production.inc"
#undef malloc
#undef free
#define PH_BONJOUR_LAN_CODEC_ONLY 1
#include "common/bonjour_lan.inc"
int main(void){
 mdns_init_t init={"_easylink_config._tcp.local.","host.local.","factory","name=factory.",8000};assert(!mdns_add_record(init,Station,120)&&live==4);
 init.service_name="_openiot._udp.local.";init.service_port=11288;assert(!mdns_add_record(init,Station,120)&&live==8);
 dns_sd_service_record_t before=available_services[1];
 for(int i=0;i<4;i++){allowance=i;assert(mdns_add_record(init,Station,120)==kNoMemoryErr);assert(!memcmp(&before,&available_services[1],sizeof(before))&&live==8&&!bonjour_mutex);}
 allowance=-1;_bonjour_announce_handler=NULL;thread_fail=true;assert(mdns_add_record(init,Station,120)==-9&&live==8&&!memcmp(&before,&available_services[1],sizeof(before)));thread_fail=false;
 assert(!mdns_add_record(init,Station,120)&&live==8);available_services[0].state=RECORD_SUSPEND;available_services[1].state=RECORD_SUSPEND;
 mdns_resume_record(init.service_name,Station);assert(available_services[1].state==RECORD_UPDATE&&available_services[0].state==RECORD_SUSPEND);
 available_services[1].state=RECORD_SUSPEND;mdns_resume_record(init.service_name,Soft_AP);assert(available_services[1].state==RECORD_SUSPEND);
 char output[384];assert(bonjour_lan_txt(output,sizeof(output),"aabbccddeeff","a1","fresh-air","1.0/rc4",11288));assert(strstr(output,"firmware=1/.0//rc4."));size_t length=strlen(output);
 for(size_t cap=1;cap<=length;cap++){char guarded[386];memset(guarded,0xa5,sizeof(guarded));assert(!bonjour_lan_txt(guarded+1,cap,"aabbccddeeff","a1","fresh-air","1.0/rc4",11288));assert((unsigned char)guarded[0]==0xa5&&(unsigned char)guarded[cap+1]==0xa5);}
 assert(!bonjour_lan_txt(output,sizeof(output),"AABBCCDDEEFF","a1","full","4",11288));assert(!bonjour_lan_txt(output,sizeof(output),"aabbccddeeff","unknown","full","4",11288));
 for(unsigned i=0;i<4;i++)_clean_record_resource(&available_services[i]);assert(!live);puts("actual SDK mDNS: 4 OOMs/thread failure/atomic record/multi-record resume/TXT canaries PASS");return 0;
}

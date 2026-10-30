/* Production scan/config state with adversarial SDK timing and byte SSIDs. */
#include <assert.h>
#include "../src/ph_port.c"
void bl_mqtt_start(void){}
void bl_mqtt_diagnostics(ph_writer *w){(void)w;}
static uint32_t clock_ms;
static size_t heap_bytes=13144;
uint32_t __wrap_rtos_get_time(void){return clock_ms;}
size_t __wrap_xPortGetFreeHeapSize(void){return heap_bytes;}
static unsigned starts;
static bool probe_start;
static void (*completion)(void *,uint8_t);
void __wrap_bk_wlan_scan_ap_reg_cb(void (*cb)(void *,uint8_t)){completion=cb;}
void __wrap_bk_wlan_start_scan(void){
 ++starts;
 /* SDK initialization must not hold the mutex needed by HTTP/buttons. */
 if(probe_start){assert(!pthread_mutex_trylock(&mutex));pthread_mutex_unlock(&mutex);}
}
struct scanu_rst_upload *__wrap_sr_get_scan_results(void){assert(!"manual scan borrowed supplicant results");return NULL;}
int __wrap_bk_wlan_get_scan_ap_result_numbers(void){assert(!"manual scan queried supplicant results");return 0;}
void __wrap_sr_release_scan_results(struct scanu_rst_upload *p){(void)p;assert(!"manual scan freed supplicant results");}
void __wrap_CFG_Save_IfThereArePendingChanges(void){clock_ms+=4000;}
static const char *action(const char *name){
 const unsigned char empty=0x80;ph_value p;assert(phv_parse(PH_WIRE_MSGPACK,&empty,1,&p));
 unsigned char out[4096];ph_writer w;struct printbuf b;
 assert(phw_init_fixed(&w,PH_WIRE_MSGPACK,&b,out,sizeof(out)));
 return platform_action(NULL,name,&p,&w,true);
}
static void beacon(const unsigned char *ssid,unsigned n,int rssi){
 unsigned char frame[36+2+33+3]={0};assert(n<=33);
 frame[34]=16;frame[36]=0;frame[37]=n;memcpy(frame+38,ssid,n);
 frame[38+n]=3;frame[39+n]=1;frame[40+n]=6;
 PH_BL_ScanResult(frame,41+n,rssi);
 memset(frame,0xff,sizeof(frame)); /* The application must own copied fields. */
}
static void scan_tests(void){
 heap_bytes=PH_SCAN_RUNNING_HEAP-1;assert(!strcmp(action("wifi.scan"),"busy"));assert(!scan_state);
 heap_bytes=13144;
 assert(!strcmp(action("wifi.scan"),"accepted"));
 scan_step(100);assert(starts==1);
 PH_BL_ScanResult(NULL,0,0);
 unsigned char malformed[40]={0};
 for(unsigned i=0;i<36;++i)PH_BL_ScanResult(malformed,i,0);
 PH_BL_ScanResult(malformed,37,0);malformed[37]=33;PH_BL_ScanResult(malformed,40,0);
 const unsigned char bad[]={0xff,0xfe},nul[]={1,0,2},hidden[]={0};
 beacon(bad,sizeof(bad),-1);beacon(nul,sizeof(nul),-1);beacon(hidden,0,-1);
 assert(!scan_collect_count);
 for(unsigned i=0;i<64;++i){char ssid[33];int n=snprintf(ssid,sizeof(ssid),"network-%u",i);beacon((unsigned char*)ssid,n,-100+(int)i);}
 assert(scan_collect_count==20&&!scan_count); /* Publish only on completion. */
 beacon((unsigned char*)"network-63",10,-2);assert(scan_collect_count==20);
 unsigned char full[33];memset(full,'a',sizeof(full));beacon(full,33,-1);assert(scan_collect_count==20);
 beacon(full,32,-1);assert(scan_collect_count==20);
 scan_step(7100);assert(scan_state==3); /* Timeout never borrows SDK storage. */
 assert(!strcmp(action("wifi.scan"),"busy")); /* No overlapping scan after timeout. */
 assert(completion);completion(NULL,0);scan_step(7200);
 assert(scan_state==0&&scan_count==20&&strlen(networks[0].ssid)==32&&networks[0].rssi==-1);
 assert(!strcmp(networks[1].ssid,"network-63")&&networks[1].rssi==-2);
 for(unsigned i=1;i<scan_count;++i)assert(networks[i-1].rssi>=networks[i].rssi);
 unsigned saved=scan_count;beacon((unsigned char*)"too-late",8,0);assert(scan_count==saved);completion(NULL,0);assert(scan_count==saved);
 unsigned char out[4096];ph_writer w;struct printbuf b;
 assert(phw_init_fixed(&w,PH_WIRE_MSGPACK,&b,out,sizeof(out)));phw_begin(&w,NULL,false);
 const unsigned char empty=0x80;ph_value p;assert(phv_parse(PH_WIRE_MSGPACK,&empty,1,&p));
 assert(!strcmp(platform_action(NULL,"wifi.scan.get",&p,&w,false),"ok"));phw_end(&w);
 size_t n=phw_finish_fixed(&w);assert(n&&phv_parse(PH_WIRE_MSGPACK,out,n,&p));
 probe_start=true;assert(!strcmp(action("wifi.scan"),"accepted"));scan_step(7300);
 assert(!scan_count&&!scan_collect_count);completion(NULL,0);scan_step(7400);assert(starts==2&&!scan_count);
 puts("PASS: borrowed scan frames are copied into a bounded strongest-20 UTF-8 metadata list; invalid IE/SSID lengths rejected; no supplicant storage touched on timeout/completion; scan start leaves product mutex available");
}
static void config_test(void){
 unsigned char out[512];ph_writer w;struct printbuf b;
 assert(phw_init_fixed(&w,PH_WIRE_MSGPACK,&b,out,sizeof(out)));phw_begin(&w,NULL,false);
 phw_text(&w,"section","wifi");phw_begin(&w,"values",false);phw_text(&w,"ssid","test-network");phw_text(&w,"password","test-password");phw_end(&w);phw_end(&w);
 ph_value p;assert(phv_parse(PH_WIRE_MSGPACK,out,phw_finish_fixed(&w),&p));
 clock_ms=10000;assert(!strcmp(platform_action(NULL,"config.set",&p,&w,true),"accepted"));
 assert(reboot_pending&&clock_ms==14000&&(int32_t)(reboot_at-clock_ms)==3000);
 assert(!strcmp(CFG_GetWiFiSSID(),"test-network")&&!strcmp(CFG_GetWiFiPass(),"test-password"));
 puts("PASS: slow native config save completes before the reboot response grace starts");
}
int main(void){assert(!rtos_init_mutex(&mutex));scan_tests();config_test();}

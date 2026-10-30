/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifdef PH_HOST_TEST
#include "platform_host.h"
#else
#include "../../new_common.h"
#include "../../new_cfg.h"
#include "../../hal/hal_pins.h"
#include "../../hal/hal_wifi.h"
#include "../../hal/hal_generic.h"
#include "../../littlefs/our_lfs.h"
#include "../../driver/drv_uart.h"
#include "rw_pub.h"
extern void set_printf_port(unsigned char);
#endif
#include "port.h"
#include "device.h"
#include "udp.h"
#include "button.h"
#include "indicator.h"
#include "port_internal.h"
#include "factory_mac.h"
#include "boot_trace.h"
#include "memory_policy.h"
#include "../protocol/httpd/http.h"
#include "../protocol/diagnostics_ids.h"
extern void OTA_RequestDownloadFromHTTP(const char *);
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#ifndef PH_HOST_TEST
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#endif
/* Forum establishes GPIO numbers; polarity is configurable pending bench QA. */
#ifndef PH_RELAY_ACTIVE
#define PH_RELAY_ACTIVE 1
#endif
#ifndef PH_RED_ACTIVE
#define PH_RED_ACTIVE 1
#endif
#ifndef PH_BLUE_ACTIVE
#define PH_BLUE_ACTIVE 1
#endif
#ifndef PH_BUTTON_ACTIVE
#define PH_BUTTON_ACTIVE 0
#endif
#define BUTTON_PIN 6
#define BLUE_PIN 7
#define RED_PIN 8
#define RELAY_PIN 9
/* Stock S30-V1B receives HLW8032 frames on UART2 RX/P1. UART1 is VTX/VRX. */
#define METER_UART_PORT UART_PORT_INDEX_1
extern OSStatus BkRandomNumberRead(void *,int);
ph_device bl_device;
ph_auth_state bl_auth;
volatile bool bl_ready;
unsigned bl_mqtt_status;
bool bl_ota_pending, bl_mqtt_quiescent=true;
static ph_udp_state udp_state;
static beken_mutex_t mutex;
static unsigned reset_requested;
static uint32_t reboot_at, energy_saved_at, ota_at;
static bool reboot_pending, ota_failed;
static char ota_url[128];
static unsigned char udp_in[PH_INPUT_MAX+1],udp_out[PH_OUTPUT_MAX];
static int udp_fd=-1;
static unsigned scan_state,scan_count;
static uint32_t scan_at;
static volatile bool scan_done;
static bool safe_boot;
static struct {char ssid[33];int rssi,security;} networks[20];
void bl_lock(void) { rtos_lock_mutex(&mutex); }
void bl_unlock(void) { rtos_unlock_mutex(&mutex); }
#define lock bl_lock
#define unlock bl_unlock
const char *PH_BL_APName(void) {
    static char name[24]; unsigned char bytes[6];
    WiFI_GetMacAddress((char *)bytes);
    ph_mac_ap_name(name,bytes);
    return name;
}
static uint32_t now(void *unused) { (void)unused; return rtos_get_time(); }
static bool random_bytes(void *unused, uint8_t *p, size_t n) {
    (void)unused;
    /* SDK BkRandomNumberRead fills at most four bytes per call. */
    while (n) { size_t k=n>4?4:n; if (BkRandomNumberRead(p,(int)k)) return false; p+=k; n-=k; }
    return true;
}
static uint32_t crc32(const void *data, size_t n) {
    uint32_t crc=UINT32_MAX; const unsigned char *p=data;
    while(n--) { crc ^= *p++; for(unsigned k=0;k<8;++k) crc=(crc>>1)^((crc&1)?UINT32_C(0xedb88320):0); }
    return ~crc;
}
typedef struct { uint32_t magic,size,crc; ph_settings settings; } disk_record;
/* Explicit v1 layout; retain metering calibration, grants and power-on policy. */
typedef struct {
 uint32_t magic,revision;uint8_t child_lock,indicator,power_on,calibrated;
 double coefficients[3],energy_kwh;ph_auth_store auth;
} legacy_settings;
typedef struct {uint32_t magic,size,crc;legacy_settings settings;} legacy_record;
static bool save(void *unused, const ph_settings *s) {
    (void)unused; if (!lfs_present() || !ph_settings_valid(s)) return false;
    disk_record r; memset(&r,0,sizeof(r)); r.magic=PH_STORE_MAGIC; r.size=sizeof(r); r.settings=*s;
    r.crc=crc32(&r.settings,sizeof(r.settings)); lfs_file_t f;
    if(lfs_file_open(&lfs,&f,".phicomm.tmp",LFS_O_WRONLY|LFS_O_CREAT|LFS_O_TRUNC)<0)return false;
    bool ok=lfs_file_write(&lfs,&f,&r,sizeof(r))==(lfs_ssize_t)sizeof(r);
    if(ok)ok=lfs_file_sync(&lfs,&f)==0;
    if(lfs_file_close(&lfs,&f)<0)ok=false;
    if(ok)ok=lfs_rename(&lfs,".phicomm.tmp",".phicomm")==0;
    return ok;
}
static bool load(ph_settings *s) {
    disk_record r; lfs_file_t f;
    if(!lfs_present() || lfs_file_open(&lfs,&f,".phicomm",LFS_O_RDONLY)<0)return false;
    lfs_soff_t size=lfs_file_size(&lfs,&f);bool ok=false;
    if(size==sizeof(r)) {
        ok=lfs_file_read(&lfs,&f,&r,sizeof(r))==sizeof(r) && r.magic==PH_STORE_MAGIC &&
           r.size==sizeof(r) && r.crc==crc32(&r.settings,sizeof(r.settings)) && ph_settings_valid(&r.settings);
        if(ok)*s=r.settings;
    } else if(size==sizeof(legacy_record)) {
        legacy_record old;
        if(lfs_file_read(&lfs,&f,&old,sizeof(old))==sizeof(old) && old.magic==UINT32_C(0x314c4250) &&
           old.size==sizeof(old) && old.settings.magic==old.magic && old.crc==crc32(&old.settings,sizeof(old.settings))) {
            ph_settings_default(s);s->revision=old.settings.revision;
            s->child_lock=old.settings.child_lock;s->indicator=old.settings.indicator;
            s->power_on=old.settings.power_on;s->calibrated=old.settings.calibrated;
            memcpy(s->coefficients,old.settings.coefficients,sizeof(s->coefficients));
            s->energy_kwh=old.settings.energy_kwh;s->auth=old.settings.auth;ok=ph_settings_valid(s);
        }
    }
    lfs_file_close(&lfs,&f);return ok;
}
static bool auth_save(void *unused, const ph_auth_store *store) {
    ph_settings next=bl_device.settings; next.auth=*store;next.energy_kwh=bl_device.meter.energy_kwh;
    if(!save(unused,&next))return false;
    bl_device.settings=next; return true;
}
static bool password(void *unused, char out[33]) {
    (void)unused; const char *p=CFG_GetWebPassword(); size_t n=strlen(p);
    if(n>32)return false; memcpy(out,p,n+1); return true;
}
int PH_BL_PasswordChanged(void) {
    if(!bl_ready)return 1;
    lock(); bool ok=ph_auth_password_changed(&bl_auth); memset(&udp_state,0,sizeof(udp_state)); unlock();
    return ok;
}
static void set_relay(void *unused, bool on) { (void)unused; HAL_PIN_SetOutputValue(RELAY_PIN,on?PH_RELAY_ACTIVE:!PH_RELAY_ACTIVE); }
static void reboot(void *unused) { (void)unused; reboot_pending=true; reboot_at=now(NULL)+3000; }
/* Caller holds the product mutex. Checkpoint without wearing flash per frame. */
static void checkpoint(uint32_t t, bool force) {
    if(!force && (uint32_t)(t-energy_saved_at)<300000)return;
    energy_saved_at=t;
    if(bl_device.settings.energy_kwh==bl_device.meter.energy_kwh)return;
    ph_settings next=bl_device.settings;next.energy_kwh=bl_device.meter.energy_kwh;
    if(save(NULL,&next))bl_device.settings=next;
}
static void network_info(void *unused, ph_writer *w) {
    (void)unused; phw_text(w,"ip",HAL_GetMyIPString()); phw_int(w,"rssi",HAL_GetWifiStrength());
    phw_bool(w,"wifi_connected",Main_IsConnectedToWiFi());
    phw_bool(w,"safe_mode",safe_boot);
    phw_bool(w,"mqtt_connected",bl_mqtt_status==MQTT_READY);
    phw_text(w,"mqtt_status",mqtt_status_name(bl_mqtt_status));
    phw_text(w,"ssid",CFG_GetWiFiSSID());phw_bool(w,"wifi_password_set",*CFG_GetWiFiPass()!=0);
    phw_bool(w,"ap_active",Main_IsOpenAccessPointMode());phw_text(w,"ap_name",PH_BL_APName());
    phw_text(w,"hostname",PH_BL_HostName());
    phw_bool(w,"scan_busy",scan_state!=0);phw_text(w,"wifi_trial_status","idle");phw_bool(w,"wifi_trial_active",false);
    phw_text(w,"mqtt_host",CFG_GetMQTTHost());phw_int(w,"mqtt_port",CFG_GetMQTTPort());
    phw_text(w,"mqtt_user",CFG_GetMQTTUserName());phw_text(w,"mqtt_topic_mode","tmcp");
    phw_bool(w,"mqtt_password_set",*CFG_GetMQTTPass()!=0);phw_bool(w,"web_password_set",*CFG_GetWebPassword()!=0);
    phw_bool(w,"storage_ready",lfs_present()!=0);
    int done=OTA_GetProgress(),total=OTA_GetTotalBytes();
    uint32_t progress=done>0&&total>0?(uint32_t)((uint64_t)done*100/total):0;
    if(progress>100)progress=100;
    phw_bool(w,"ota_active",bl_ota_pending);phw_text(w,"ota_state",done>=0?(progress==100?"rebooting":"downloading"):*ota_url?"pending":ota_failed?"failed":"idle");
    phw_u32(w,"ota_progress",progress);
}
static uint32_t network_stamp(void *unused) {
    (void)unused;uint32_t h=2166136261u;
    const unsigned char *p=(const unsigned char*)HAL_GetMyIPString();
    while(*p)h=(h^*p++)*16777619u;
    uint32_t flags=((uint32_t)HAL_GetWifiStrength()&255u)^(Main_IsConnectedToWiFi()?1u<<9:0)^
        (Main_IsOpenAccessPointMode()?1u<<10:0)^(scan_state<<11)^(bl_mqtt_status<<14)^
        (bl_ota_pending?1u<<18:0)^(ota_failed?1u<<19:0)^(*ota_url?1u<<20:0)^
        (lfs_present()?1u<<21:0)^(*CFG_GetWebPassword()?1u<<22:0);
    int done=OTA_GetProgress(),total=OTA_GetTotalBytes();
    uint32_t progress=done>0&&total>0?(uint32_t)((uint64_t)done*100/total):0;
    return h^flags^(progress<<24)^(done>=0?1u<<23:0);
}
static void diagnostics(void *unused,ph_writer *w) {
    (void)unused;ph_http_diagnostics h=ph_http_snapshot();
    ph_diag_uint(w,PH_DIAG_FREE_MEMORY,xPortGetFreeHeapSize());
    ph_diag_uint(w,PH_DIAG_HTTP_ACCEPT_OK,h.accepted);
    ph_diag_uint(w,PH_DIAG_HTTP_ACCEPT_QUEUE_OK,h.queued);
    ph_diag_uint(w,PH_DIAG_HTTP_ACCEPT_QUEUE_REJECT,h.rejected);
    ph_diag_uint(w,PH_DIAG_HTTP_ACCEPT_QUEUE_CAPACITY,PH_HTTP_WAIT_SLOTS);
    ph_diag_uint(w,PH_DIAG_HTTP_ACCEPT_BUSY_SEND_OK,h.busy_sent);
    ph_diag_uint(w,PH_DIAG_HTTP_ACCEPT_BUSY_SEND_FAIL,h.busy_failed);
    bl_mqtt_diagnostics(w);
}
void PH_BL_Tick(void) {
    if(!bl_ready)return;
    PH_BOOT_PULSE("tick-enter");
    static ph_button button;
    uint32_t t=now(NULL); bool raw=HAL_PIN_ReadDigitalInput(BUTTON_PIN)==PH_BUTTON_ACTIVE;
    lock();
    int count=UART_GetDataSizeEx(METER_UART_PORT);
    if(count>128)count=128;
    for(int i=0;i<count;++i)ph_meter_byte(&bl_device.meter,UART_GetByteEx(METER_UART_PORT,i),t,bl_device.settings.coefficients);
    UART_ConsumeBytesEx(METER_UART_PORT,count);
    ph_button_event event=ph_button_tick(&button,raw,t);
    if(event==PH_BUTTON_CLICK)ph_local_toggle(&bl_device);
    else if(event==PH_BUTTON_WIFI)reset_requested=1;
    else if(event==PH_BUTTON_RESET)reset_requested=2;
    ph_indicator lights=ph_indicator_state(t,bl_device.settings.indicator,
        bl_device.relay,*CFG_GetWiFiSSID()||*CFG_GetWiFiSSID2(),Main_IsConnectedToWiFi(),
        *CFG_GetMQTTHost()!=0,bl_mqtt_status==MQTT_READY,button.down&&button.raw,(uint32_t)(t-button.pressed));
    HAL_PIN_SetOutputValue(RED_PIN,lights.red?PH_RED_ACTIVE:!PH_RED_ACTIVE);
    HAL_PIN_SetOutputValue(BLUE_PIN,lights.blue?PH_BLUE_ACTIVE:!PH_BLUE_ACTIVE);
    unlock();
    PH_BOOT_PULSE("tick-done");
}
static bool get_text(const ph_value *p,const char *key,char *out,size_t cap) {
 ph_value v;return phv_get(p,key,&v)&&phv_text(&v,out,cap);
}
static const char *platform_action(void *unused,const char *action,const ph_value *p,ph_writer *w,bool admin) {
 (void)unused;ph_value values,v;char section[24],a[128]={0},b[128]={0},c[128]={0};int64_t port;
 if(!strcmp(action,"wifi.scan.get")) {
  if(p->count)return "invalid_params";
  phw_bool(w,"scanning",scan_state!=0);phw_begin(w,"networks",true);
  for(unsigned i=0;i<scan_count;++i){phw_begin(w,NULL,false);phw_text(w,"ssid",networks[i].ssid);phw_int(w,"rssi",networks[i].rssi);phw_int(w,"security",networks[i].security);phw_end(w);}
  phw_end(w);return "ok";
 }
 if(!admin)return "unauthorized";
 if(!strcmp(action,"wifi.scan")) {
  if(p->count)return "invalid_params";
  if(bl_ota_pending)return "busy";
  if(!scan_state){scan_state=1;scan_count=0;}return "accepted";
 }
 if(!strcmp(action,"system.reset")) {
  bool full=false;
  if(!phv_keys(p,"|full|")||!phv_get(p,"full",&v)||!phv_bool(&v,&full))return "invalid_params";
  reset_requested=full?2:1;reboot_at=now(NULL)+3000;return "accepted";
 }
 if(!strcmp(action,"ota.start")) {
  if(p->count!=1||!get_text(p,"url",a,sizeof(a))||strncmp(a,"http://",7)||strpbrk(a,"\r\n\t ")||strlen(a)<12)return "invalid_params";
  if(bl_ota_pending||OTA_GetProgress()>=0||reboot_pending||reset_requested||scan_state)return "busy";
  strcpy(ota_url,a);ota_at=now(NULL)+1500;bl_ota_pending=true;bl_device.maintenance=true;ota_failed=false;return "accepted";
 }
 if(strcmp(action,"config.set"))return "unsupported";
 if(!get_text(p,"section",section,sizeof(section))||!phv_get(p,"values",&values)||values.kind!=PHV_MAP)return "invalid_params";
 if(!strcmp(section,"wifi")) {
  bool has_pass=phv_get(&values,"password",&v);
  if(!phv_keys(&values,"|ssid||password|")||!get_text(&values,"ssid",a,33)||!*a)return "invalid_params";
  if(has_pass&&(!get_text(&values,"password",b,64)||(*b&&strlen(b)<8)))return "invalid_params";
  CFG_SetWiFiSSID(a);if(has_pass)CFG_SetWiFiPass(b);reboot(NULL);
 } else if(!strcmp(section,"mqtt")) {
  bool has_pass=phv_get(&values,"password",&v);
  if(!phv_keys(&values,"|host||port||user||password||protocol|")||!get_text(&values,"host",a,sizeof(a))||
     !get_text(&values,"user",b,64)||!phv_get(&values,"port",&v)||!phv_int(&v,&port)||port<1||port>65535)return "invalid_params";
  if(phv_get(&values,"protocol",&v)&&!phv_equal_text(&v,"tmcp"))return "invalid_params";
  for(const char *s=a;*s;++s)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='.'||*s=='-'))return "invalid_params";
  if(has_pass&&!get_text(&values,"password",c,sizeof(c)))return "invalid_params";
  CFG_SetMQTTHost(a);CFG_SetMQTTPort((int)port);CFG_SetMQTTUserName(b);if(has_pass)CFG_SetMQTTPass(c);
 } else if(!strcmp(section,"auth")) {
  if(values.count!=1||!get_text(&values,"password",a,33)||(*a&&strlen(a)<8))return "invalid_params";
  for(const char *s=a;*s;++s)if((unsigned char)*s<32||(unsigned char)*s>126)return "invalid_params";
  /* Native CFG calls the durable grant revocation hook with its own lock. */
  unlock();CFG_SetWebPassword(a);lock();
  if(strcmp(CFG_GetWebPassword(),a))return "storage_error";
 } else return "unsupported";
 memset(a,0,sizeof(a));memset(b,0,sizeof(b));memset(c,0,sizeof(c));
 CFG_Save_IfThereArePendingChanges();return "accepted";
}
static void scan_callback(void *context,uint8_t param) {(void)context;(void)param;scan_done=true;}
static void scan_step(uint32_t t) {
 if(scan_state==1) {
  scan_done=false;scan_at=t;scan_state=2;
  bk_wlan_scan_ap_reg_cb(scan_callback);bk_wlan_start_scan();
 } else if(scan_state==2&&(scan_done||(uint32_t)(t-scan_at)>=7000)) {
  struct scanu_rst_upload *results=sr_get_scan_results();
  int count=bk_wlan_get_scan_ap_result_numbers();
  scan_count=0;
  if(results)for(int i=0;i<count&&scan_count<20;++i) {
   if(!results->res[i])continue;
   memcpy(networks[scan_count].ssid,results->res[i]->ssid,32);networks[scan_count].ssid[32]=0;
   if(!*networks[scan_count].ssid)continue;
   networks[scan_count].rssi=results->res[i]->level;
   networks[scan_count].security=results->res[i]->security;scan_count++;
  }
  if(results)sr_release_scan_results(results);
  scan_state=0;
 }
}
static void network_thread(beken_thread_arg_t arg) {
 (void)arg;
 PH_BOOT_STAGE("lan-thread");
 for(;;) {
  uint32_t t=now(NULL);unsigned reset=0;char url[128]="";bool restart=false;
  lock();if(!bl_ota_pending)scan_step(t);
  if(!bl_ota_pending&&(int32_t)(t-reboot_at)>=0) {
   restart=reboot_pending;reset=reset_requested;reset_requested=0;
  }
  checkpoint(t,restart||reset==1);
  if(reset==2) {
   ph_settings defaults;ph_settings_default(&defaults);
   if(!save(NULL,&defaults))reset=0;
   else {bl_device.settings=defaults;set_relay(NULL,false);}
  }
  if(bl_ota_pending) {
   if(*ota_url&&(int32_t)(t-ota_at)>=0) {
    /* The shared owner delivers its captured command receipt before closing.
     * Wait for STOPPED (after buffer/socket teardown), with a bounded grace. */
    if(bl_mqtt_quiescent&&xPortGetFreeHeapSize()>=PH_OTA_START_HEAP) {
     strcpy(url,ota_url);ota_url[0]=0;
    } else if((uint32_t)(t-ota_at)>=10000) {
     ota_url[0]=0;bl_ota_pending=false;bl_device.maintenance=false;ota_failed=true;
    }
   } else if(!*ota_url&&OTA_GetProgress()<0) {
    bl_ota_pending=false;bl_device.maintenance=false;ota_failed=true;
   }
  }
  bool upgrading=bl_ota_pending;
  unlock();
  if(reset) {
   if(reset==2){CFG_SetDefaultConfig();CFG_SetMQTTUserName("");CFG_SetMQTTPass("");CFG_SetDeviceName("S30-V1B");}
   CFG_SetWiFiSSID("");CFG_SetWiFiPass("");CFG_SetWiFiSSID2("");CFG_SetWiFiPass2("");
   CFG_Save_IfThereArePendingChanges();HAL_RebootModule();
  }
  if(restart)HAL_RebootModule();
  if(upgrading) {
   if(udp_fd>=0){close(udp_fd);udp_fd=-1;}
   if(*url)OTA_RequestDownloadFromHTTP(url);
   rtos_delay_milliseconds(100);continue;
  }
  if(udp_fd<0) {
   udp_fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
   if(udp_fd>=0){struct sockaddr_in a;memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons(11288);a.sin_addr.s_addr=INADDR_ANY;
#ifdef PH_HOST_TEST
    a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
#endif
    if(bind(udp_fd,(struct sockaddr*)&a,sizeof(a))){close(udp_fd);udp_fd=-1;}}
  }
  fd_set reads;FD_ZERO(&reads);if(udp_fd>=0)FD_SET(udp_fd,&reads);
  struct timeval delay={0,100000};int events=select(udp_fd+1,&reads,NULL,NULL,&delay);
  if(events>0&&udp_fd>=0&&FD_ISSET(udp_fd,&reads)) {
   struct sockaddr_in remote;socklen_t size=sizeof(remote);
   int n=recvfrom(udp_fd,udp_in,sizeof(udp_in),0,(struct sockaddr*)&remote,&size);
   if(n>0&&n<=PH_INPUT_MAX){lock();size_t k=ph_udp_handle(&udp_state,&bl_device,&bl_auth,udp_in,n,udp_out,sizeof(udp_out));unlock();
    /* The lab's LAN transport listens and replies on the shared service port. */
    remote.sin_port=htons(11288);if(k)sendto(udp_fd,udp_out,k,0,(struct sockaddr*)&remote,size);}
  }
 }
}
void PH_BL_Init(int safe_mode) {
 if(bl_ready)return;
 /* SDK log-port numbers are one-based, unlike UART_PORT_INDEX_* enums.
  * Release UART2 for the meter before configuring its baud/parity/callback. */
 set_printf_port(1);
 PH_BOOT_RESULT("product-init",safe_mode);
 HAL_PIN_Setup_Output_Initial(RELAY_PIN,!PH_RELAY_ACTIVE);
 HAL_PIN_Setup_Output_Initial(RED_PIN,!PH_RED_ACTIVE);HAL_PIN_Setup_Output_Initial(BLUE_PIN,!PH_BLUE_ACTIVE);
 HAL_PIN_Setup_Input_Pullup(BUTTON_PIN);
 int result=rtos_init_mutex(&mutex);PH_BOOT_RESULT("product-mutex",result);if(result)return;
 if(!lfs_present())init_lfs(1);
 PH_BOOT_RESULT("storage-mounted",lfs_present());
 ph_settings settings;if(!load(&settings))ph_settings_default(&settings);
 PH_BOOT_STAGE("settings-loaded");
 bool power_on=settings.power_on;
 safe_boot=safe_mode!=0;
 if(safe_boot)settings.power_on=false;
 char mac[13];unsigned char bytes[6];WiFI_GetMacAddress((char*)bytes);
 PH_BOOT_RESULT("identity-valid",ph_mac_valid(bytes));
 if(!ph_mac_valid(bytes))return;
 snprintf(mac,sizeof(mac),"%02x%02x%02x%02x%02x%02x",bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5]);
 uint32_t boot=0;if(!random_bytes(NULL,(uint8_t*)&boot,sizeof(boot))||!boot)return;
 PH_BOOT_STAGE("random-ready");
 ph_device_ops ops={NULL,now,save,set_relay,reboot,network_info,platform_action,diagnostics,network_stamp};
 if(!ph_device_init(&bl_device,&ops,&settings,mac,USER_SW_VER,boot))return;
 PH_BOOT_STAGE("device-ready");
 /* Recovery starts with the relay off without erasing the saved boot policy. */
 bl_device.settings.power_on=power_on;
 ph_auth_ops ao={NULL,now,random_bytes,NULL,NULL,password,auth_save};
 if(!ph_auth_init(&bl_auth,&ao,&bl_device.settings.auth))return;
 PH_BOOT_STAGE("auth-ready");
 UART_InitReceiveRingBufferEx(METER_UART_PORT,512);
 PH_BOOT_RESULT("meter-buffer",UART_GetReceiveRingBufferSizeEx(METER_UART_PORT));
 if(UART_GetReceiveRingBufferSizeEx(METER_UART_PORT)!=512){set_relay(NULL,false);return;}
 UART_InitUARTEx(METER_UART_PORT,4800,2,false);
 PH_BOOT_STAGE("meter-uart-ready");
 bl_ready=true;
 result=rtos_create_thread(NULL,BEKEN_APPLICATION_PRIORITY,"openiot-lan",network_thread,16384,0);
 PH_BOOT_RESULT("lan-created",result);
 if(result){bl_ready=false;set_relay(NULL,false);return;}
 bl_mqtt_start();
 PH_BOOT_STAGE("product-ready");
}

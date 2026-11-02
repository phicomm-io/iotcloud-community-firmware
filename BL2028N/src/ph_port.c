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
#ifdef PH_S30_LAN_V4
#include "ph_lan_v4.h"
#include "ph_mdns.h"
#include "ph_sha256.h"
#else
#include "udp.h"
#endif
#include "button.h"
#include "indicator.h"
#include "port_internal.h"
#include "factory_mac.h"
#include "boot_trace.h"
#include "memory_policy.h"
#include "storage_legacy.h"
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
#ifdef PH_S30_LAN_V4
static ph_lan_v4 udp_state;
#else
static ph_udp_state udp_state;
#endif
static beken_mutex_t mutex;
static unsigned reset_requested;
static uint32_t reboot_at, energy_saved_at, ota_at;
static bool reboot_pending, ota_failed;
static char ota_url[128];
#ifdef PH_S30_LAN_V4
/* One synchronous network owner; parsed views expire before this buffer is
 * reused as the reply frame, and no owner writes it between unlock/sendto. */
static unsigned char udp_in[LAN_V4_FRAME_MAX+1];
#else
static unsigned char udp_in[PH_INPUT_MAX+1],udp_out[PH_OUTPUT_MAX];
#endif
static int udp_fd=-1;
static volatile unsigned scan_state;
static unsigned scan_count,scan_collect_count;
static uint32_t scan_at;
static bool safe_boot;
/* A password transaction is authoritative in the same atomic product record
 * as its grants. Legacy records keep using the framework's existing value. */
static bool credential_owned;
static char committed_password[33];
typedef struct {char ssid[33];int rssi,security;} scan_network;
static scan_network networks[20];
extern int get_security_type_from_ie(unsigned char *,int,unsigned short);
extern unsigned char rw_ieee80211_is_scan_rst_in_countrycode(unsigned char);
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
#define PH_CREDENTIAL_MAGIC UINT32_C(0x31445750)
typedef struct {
    uint32_t magic,size,crc;ph_settings settings;
    uint32_t credential_magic;char password[33];uint8_t reserved[3];
} disk_record;
/* PBL4 is a fixed disk ABI. A later auth/settings expansion needs another
 * explicit importer rather than silently reinterpreting this record. */
typedef char ph_store_v4_auth_size[(sizeof(ph_auth_store)==780)?1:-1];
typedef char ph_store_v4_settings_size[(sizeof(ph_settings)==1096)?1:-1];
typedef char ph_store_v4_record_size[(sizeof(disk_record)==1152)?1:-1];
typedef char ph_store_v4_auth_offset[(offsetof(ph_settings,auth)==48)?1:-1];
typedef char ph_store_v4_report_offset[(offsetof(ph_settings,report_seconds)==828)?1:-1];
typedef char ph_store_v4_name_offset[(offsetof(ph_settings,name)==832)?1:-1];
typedef char ph_store_v4_socket_offset[(offsetof(ph_settings,socket_name)==896)?1:-1];
typedef char ph_store_v4_payload_offset[(offsetof(disk_record,settings)==16)?1:-1];
typedef char ph_store_v4_credential_offset[(offsetof(disk_record,credential_magic)==1112)?1:-1];
typedef char ph_store_v4_password_offset[(offsetof(disk_record,password)==1116)?1:-1];
static bool password_value_valid(const char *p) {
    if(!p)return false;size_t n=0;
    for(;n<33&&p[n];++n)if((unsigned char)p[n]<32||(unsigned char)p[n]>126)return false;
    return n<=32&&(!n||n>=8);
}
static bool credential_fields_valid(uint32_t magic,const char password[33],const uint8_t reserved[3]) {
    if(reserved[0]||reserved[1]||reserved[2])return false;
    if(magic==PH_CREDENTIAL_MAGIC)
        return memchr(password,0,33)&&password_value_valid(password);
    if(magic)return false;
    for(unsigned i=0;i<33;++i)if(password[i])return false;
    return true;
}
static bool record_credential_valid(const disk_record *r) {
    return credential_fields_valid(r->credential_magic,r->password,r->reserved);
}
static void import_credential(uint32_t magic,const char password[33]) {
    credential_owned=magic==PH_CREDENTIAL_MAGIC;
    memset(committed_password,0,sizeof(committed_password));
    if(credential_owned) {
        memcpy(committed_password,password,sizeof(committed_password));
        PH_BL_SetWebPasswordMemory(committed_password);
    }
}
static void import_auth(ph_auth_store *to,const ph_store_old_auth *from) {
    ph_auth_store_empty(to);to->magic=from->magic;to->version=from->version;to->count=from->count;
    for(unsigned i=0;i<12;++i) {
        memcpy(to->pairs[i].peer,from->pairs[i].peer,sizeof(from->pairs[i].peer));
        memcpy(to->pairs[i].key,from->pairs[i].key,sizeof(from->pairs[i].key));
        to->pairs[i].scopes=from->pairs[i].scopes;
        memcpy(to->pairs[i].reserved,from->pairs[i].reserved,sizeof(from->pairs[i].reserved));
    }
}
/* Migration already holds the loaded record and settings on the 4 KiB app
 * stack. QuickTick also saves from a 2 KiB stack. Keep the temporary disk
 * record off both stacks; allocation failure leaves the committed file intact.
 * All callers serialize through bl_lock, or run before task startup. */
static bool save_record(const ph_settings *s,const ph_auth_store *auth,double energy,const char *password,bool owned) {
    if (!lfs_present() || !ph_settings_valid(s)) return false;
    if(owned&&!password_value_valid(password))return false;
    disk_record *r=malloc(sizeof(*r));if(!r)return false;
    memset(r,0,sizeof(*r));r->magic=PH_STORE_MAGIC;r->size=sizeof(*r);r->settings=*s;
    if(auth)r->settings.auth=*auth;r->settings.energy_kwh=energy;
    if(owned) {r->credential_magic=PH_CREDENTIAL_MAGIC;memcpy(r->password,password,strlen(password)+1);}
    if(!ph_settings_valid(&r->settings)){free(r);return false;}
    r->crc=crc32(&r->settings,sizeof(*r)-offsetof(disk_record,settings));lfs_file_t f;
    if(lfs_file_open(&lfs,&f,".phicomm.tmp",LFS_O_WRONLY|LFS_O_CREAT|LFS_O_TRUNC)<0){free(r);return false;}
    bool ok=lfs_file_write(&lfs,&f,r,sizeof(*r))==(lfs_ssize_t)sizeof(*r);
    free(r);
    if(ok)ok=lfs_file_sync(&lfs,&f)==0;
    if(lfs_file_close(&lfs,&f)<0)ok=false;
    if(ok)ok=lfs_rename(&lfs,".phicomm.tmp",".phicomm")==0;
    return ok;
}
static bool save(void *unused, const ph_settings *s) {
    (void)unused;return save_record(s,NULL,s->energy_kwh,committed_password,credential_owned);
}
static bool load(ph_settings *s) {
    union {disk_record current;ph_store_v1_record v1;ph_store_v2_record v2;ph_store_v3_record v3;} r;
    lfs_file_t f;
    if(!lfs_present() || lfs_file_open(&lfs,&f,".phicomm",LFS_O_RDONLY)<0)return false;
    lfs_soff_t size=lfs_file_size(&lfs,&f);bool ok=false,migrated=false;
    if((size==sizeof(disk_record)||size==sizeof(ph_store_v1_record)||size==sizeof(ph_store_v2_record)||size==sizeof(ph_store_v3_record)) &&
       size<=sizeof(r) && lfs_file_read(&lfs,&f,&r,(size_t)size)==size && r.current.size==size) {
        if(r.current.magic==PH_STORE_MAGIC&&size==sizeof(disk_record)) {
            ok=r.current.crc==crc32(&r.current.settings,sizeof(r.current)-offsetof(disk_record,settings))&&
               ph_settings_valid(&r.current.settings)&&record_credential_valid(&r.current);
            if(ok) {
                *s=r.current.settings;import_credential(r.current.credential_magic,r.current.password);
            }
        } else if(r.current.magic==PH_STORE_V3_MAGIC&&size==sizeof(ph_store_v3_record)&&
                  r.v3.settings.magic==PH_STORE_V3_MAGIC&&r.v3.settings.power_on<=1&&
                  r.v3.crc==crc32(&r.v3.settings,sizeof(r.v3)-offsetof(ph_store_v3_record,settings))&&
                  credential_fields_valid(r.v3.credential_magic,r.v3.password,r.v3.reserved)) {
            const ph_store_v3_settings *old=&r.v3.settings;ph_settings_default(s);
            s->revision=old->revision;s->child_lock=old->child_lock;s->indicator=old->indicator;
            s->power_on=old->power_on;s->calibrated=old->calibrated;
            memcpy(s->coefficients,old->coefficients,sizeof(s->coefficients));s->energy_kwh=old->energy_kwh;
            /* Frozen source layout is checked independently of current auth. */
            memcpy(&s->auth,&old->auth,sizeof(old->auth));s->report_seconds=old->report_seconds;
            memcpy(s->name,old->name,sizeof(s->name));memcpy(s->socket_name,old->socket_name,sizeof(s->socket_name));
            ok=ph_settings_valid(s);migrated=ok;
            if(ok)import_credential(r.v3.credential_magic,r.v3.password);
        } else if(r.current.magic==PH_STORE_V2_MAGIC&&size==sizeof(ph_store_v2_record)&&
                  r.v2.settings.magic==PH_STORE_V2_MAGIC&&r.v2.settings.power_on<=1&&r.v2.crc==crc32(&r.v2.settings,sizeof(r.v2.settings))) {
            const ph_store_v2_settings *old=&r.v2.settings;ph_settings_default(s);
            s->revision=old->revision;s->child_lock=old->child_lock;s->indicator=old->indicator;
            s->power_on=old->power_on;s->calibrated=old->calibrated;
            memcpy(s->coefficients,old->coefficients,sizeof(s->coefficients));s->energy_kwh=old->energy_kwh;
            import_auth(&s->auth,&old->auth);s->report_seconds=old->report_seconds;
            memcpy(s->name,old->name,sizeof(s->name));memcpy(s->socket_name,old->socket_name,sizeof(s->socket_name));
            ok=ph_settings_valid(s);migrated=ok;
        } else if(r.current.magic==PH_STORE_V1_MAGIC&&size==sizeof(ph_store_v1_record)&&
                  r.v1.settings.magic==PH_STORE_V1_MAGIC&&r.v1.settings.power_on<=1&&r.v1.crc==crc32(&r.v1.settings,sizeof(r.v1.settings))) {
            const ph_store_v1_settings *old=&r.v1.settings;ph_settings_default(s);
            s->revision=old->revision;s->child_lock=old->child_lock;s->indicator=old->indicator;
            s->power_on=old->power_on;s->calibrated=old->calibrated;
            memcpy(s->coefficients,old->coefficients,sizeof(s->coefficients));s->energy_kwh=old->energy_kwh;
            import_auth(&s->auth,&old->auth);ok=ph_settings_valid(s);migrated=ok;
        }
    }
    lfs_file_close(&lfs,&f);
    /* Normalize before safe-mode changes the in-memory relay policy. An
     * atomic-save failure keeps the old record intact; use corrected defaults
     * for this boot and retry next boot, without discarding any settings. */
    if(ok) {
        if(migrated&&r.current.magic!=PH_STORE_V3_MAGIC){credential_owned=false;memset(committed_password,0,sizeof(committed_password));}
        if(ph_settings_migrate_meter_defaults(s))migrated=true;if(migrated)(void)save(NULL,s);
    }
    return ok;
}
static bool auth_save(void *unused, const ph_auth_store *store) {
    (void)unused;
    if(!save_record(&bl_device.settings,store,bl_device.meter.energy_kwh,committed_password,credential_owned))return false;
    bl_device.settings.auth=*store;bl_device.settings.energy_kwh=bl_device.meter.energy_kwh;return true;
}
static bool password(void *unused, char out[33]) {
    (void)unused; const char *p=CFG_GetWebPassword(); size_t n=strlen(p);
    if(n>32)return false; memcpy(out,p,n+1); return true;
}
static bool credential_commit(void *password,const ph_auth_store *store) {
    const char *next=password;
    if(!save_record(&bl_device.settings,store,bl_device.meter.energy_kwh,next,true))return false;
    /* The durable record now owns both values. A reset or an unreported SDK
     * save failure restores this exact password before auth initialization. */
    PH_BL_SetWebPasswordMemory(next);memset(committed_password,0,sizeof(committed_password));
    memcpy(committed_password,next,strlen(next)+1);credential_owned=true;
    bl_device.settings.auth=*store;bl_device.settings.energy_kwh=bl_device.meter.energy_kwh;
    return true;
}
static bool web_password_set_locked(const char *next) {
    if(!password_value_valid(next))return false;
    if(!strcmp(CFG_GetWebPassword(),next))return true;
    bool ok=ph_auth_credential_commit(&bl_auth,next,false,credential_commit,(void *)next);
    if(ok) {
        /* This product revokes its broker-issued read grants together with
         * local credentials. A failed/no-op commit leaves them intact. */
        memset(bl_auth.mqtt,0,sizeof(bl_auth.mqtt));
#ifdef PH_S30_LAN_V4
        ph_lan_v4_revoke(&udp_state);
#else
        memset(&udp_state,0,sizeof(udp_state));
#endif
    }
    return ok;
}
int PH_BL_WebPasswordSet(const char *next) {
    if(!next||strlen(next)>32)return 0;
    if(!bl_ready){PH_BL_SetWebPasswordMemory(next);return 1;}
    lock();bool ok=web_password_set_locked(next);unlock();
    if(ok)CFG_Save_IfThereArePendingChanges();return ok;
}
static void set_relay(void *unused, bool on) { (void)unused; HAL_PIN_SetOutputValue(RELAY_PIN,on?PH_RELAY_ACTIVE:!PH_RELAY_ACTIVE); }
static void reboot(void *unused) { (void)unused; reboot_pending=true; reboot_at=now(NULL)+3000; }
/* Caller holds the product mutex. Checkpoint without wearing flash per frame. */
static bool checkpoint(uint32_t t, bool force) {
    static uint32_t attempted_at;static bool attempted;
    bool relay_dirty=bl_device.settings.power_on==PH_BOOT_RESTORE&&bl_device.settings.last_relay!=bl_device.relay;
    if(!force&&!relay_dirty&&(uint32_t)(t-energy_saved_at)<300000)return true;
    if(!relay_dirty&&bl_device.settings.energy_kwh==bl_device.meter.energy_kwh){energy_saved_at=t;return true;}
    if(!force&&attempted&&(uint32_t)(t-attempted_at)<1000)return false;
    attempted=true;attempted_at=t;
    uint8_t saved=bl_device.settings.last_relay;
    if(bl_device.settings.power_on==PH_BOOT_RESTORE)bl_device.settings.last_relay=bl_device.relay;
    bool ok=save_record(&bl_device.settings,NULL,bl_device.meter.energy_kwh,committed_password,credential_owned);
    if(!ok){bl_device.settings.last_relay=saved;return false;}
    bl_device.settings.energy_kwh=bl_device.meter.energy_kwh;energy_saved_at=t;return true;
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
 bool restart_after_save=false;
 if(!strcmp(action,"wifi.scan.get")) {
  if(p->count)return "invalid_params";
  phw_bool(w,"scanning",scan_state==1||scan_state==2);
  if(scan_state==3)phw_text(w,"error","timeout");
  phw_begin(w,"networks",true);
  for(unsigned i=0;i<scan_count;++i){phw_begin(w,NULL,false);phw_text(w,"ssid",networks[i].ssid);phw_int(w,"rssi",networks[i].rssi);phw_int(w,"security",networks[i].security);phw_end(w);}
  phw_end(w);return "ok";
 }
 if(!admin)return "unauthorized";
 if(!strcmp(action,"wifi.scan")) {
  if(p->count)return "invalid_params";
  if(bl_ota_pending)return "busy";
  /* A timeout does not cancel the radio scan. Wait for its completion rather
   * than overlap requests or free storage still owned by the SDK. */
  if(scan_state==3)return "busy";
  size_t start_heap=Main_IsConnectedToWiFi()?PH_SCAN_RUNNING_HEAP:PH_SCAN_START_HEAP;
  if(!scan_state&&xPortGetFreeHeapSize()<start_heap)return "busy";
  if(!scan_state){scan_state=1;scan_count=scan_collect_count=0;}return "accepted";
 }
 if(!strcmp(action,"system.reset")) {
  bool full=false;
  if(!phv_keys(p,"|full|")||!phv_get(p,"full",&v)||!phv_bool(&v,&full))return "invalid_params";
  reset_requested=full?2:1;reboot_at=now(NULL)+3000;return "accepted";
 }
 if(!strcmp(action,"ota.start")) {
  if(p->count!=1||!get_text(p,"url",a,sizeof(a))||strncmp(a,"http://",7)||strpbrk(a,"\r\n\t ")||strlen(a)<12)return "invalid_params";
  if(bl_ota_pending||OTA_GetProgress()>=0||reboot_pending||reset_requested||scan_state)return "busy";
  /* The downloader reboots inside the SDK rather than our normal reboot
   * path. Persist the latest energy before accepting and pausing services. */
  if(!checkpoint(now(NULL),true))return "storage_error";
  strcpy(ota_url,a);ota_at=now(NULL)+1500;bl_ota_pending=true;ph_device_set_maintenance(&bl_device,true);ota_failed=false;return "accepted";
 }
 if(strcmp(action,"config.set"))return "unsupported";
 if(!get_text(p,"section",section,sizeof(section))||!phv_get(p,"values",&values)||values.kind!=PHV_MAP)return "invalid_params";
 if(!strcmp(section,"wifi")) {
  bool has_pass=phv_get(&values,"password",&v);
  if(!phv_keys(&values,"|ssid||password|")||!get_text(&values,"ssid",a,33)||!*a)return "invalid_params";
  if(has_pass&&(!get_text(&values,"password",b,64)||(*b&&strlen(b)<8)))return "invalid_params";
  CFG_SetWiFiSSID(a);if(has_pass)CFG_SetWiFiPass(b);restart_after_save=true;
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
  if(!web_password_set_locked(a))return "storage_error";
 } else return "unsupported";
 memset(a,0,sizeof(a));memset(b,0,sizeof(b));memset(c,0,sizeof(c));
 CFG_Save_IfThereArePendingChanges();
 if(restart_after_save)reboot(NULL);
 return "accepted";
}
void PH_BL_ScanResult(const unsigned char *frame,unsigned bytes,int rssi) {
 /* Borrow a complete beacon/probe response only for this callback. Keep no
  * SDK pointer or beacon IE allocation: the UI needs three small fields. */
 if(!frame||bytes<36)return;
 const unsigned char *ssid=NULL;unsigned ssid_len=0;
 for(unsigned at=36;at<bytes;) {
  if(bytes-at<2||frame[at+1]>bytes-at-2)return;
  unsigned n=frame[at+1];
  if(frame[at]==0&&!ssid){ssid=frame+at+2;ssid_len=n;}
  if(frame[at]==3&&(n!=1||!rw_ieee80211_is_scan_rst_in_countrycode(frame[at+2])))return;
  at+=n+2;
 }
 /* 802.11 SSIDs are byte strings; our text protocol requires UTF-8. */
 if(!ssid||!ssid_len||ssid_len>32||memchr(ssid,0,ssid_len)||!ph_wire_utf8(ssid,ssid_len))return;
 int security=get_security_type_from_ie((unsigned char*)frame+36,(int)(bytes-36),
                                       frame[34]|((unsigned)frame[35]<<8));
 lock();
 if(scan_state!=2&&scan_state!=3){unlock();return;}
 unsigned slot=scan_collect_count;
 for(unsigned i=0;i<scan_collect_count;++i)
  if(strlen(networks[i].ssid)==ssid_len&&!memcmp(networks[i].ssid,ssid,ssid_len)&&networks[i].security==security){
   if(rssi>networks[i].rssi)networks[i].rssi=rssi;
   unlock();return;
  }
 if(slot==20) {
  slot=0;for(unsigned i=1;i<20;++i)if(networks[i].rssi<networks[slot].rssi)slot=i;
  if(rssi<=networks[slot].rssi){unlock();return;}
 } else ++scan_collect_count;
 memcpy(networks[slot].ssid,ssid,ssid_len);networks[slot].ssid[ssid_len]=0;
 networks[slot].rssi=rssi;networks[slot].security=security;unlock();
}
void PH_BL_ScanCompleted(void *context,uint8_t param) {
 (void)context;(void)param;lock();
 if(scan_state==2||scan_state==3) {
  for(unsigned i=0;i<scan_collect_count;++i)for(unsigned j=i+1;j<scan_collect_count;++j)
   if(networks[j].rssi>networks[i].rssi){scan_network tmp=networks[i];networks[i]=networks[j];networks[j]=tmp;}
  scan_count=scan_collect_count;scan_state=0;
 }
 unlock();
}
static void scan_step(uint32_t t) {
 bool start=false;
 lock();
 if(!bl_ota_pending&&scan_state==1) {
  scan_at=t;scan_state=2;start=true;
 } else if(scan_state==2&&(uint32_t)(t-scan_at)>=7000)scan_state=3;
 unlock();
 if(start) {
  /* Station initialization may wait on SDK tasks. Keep the local button and
   * HTTP/control state mutex available throughout that work. */
  bk_wlan_scan_ap_reg_cb(PH_BL_ScanCompleted);bk_wlan_start_scan();
 }
}
#ifdef PH_S30_LAN_V4
static size_t lan_free_heap(void *unused) {(void)unused;return xPortGetFreeHeapSize();}
static bool lan_digest(void *unused,const void *bytes,size_t n,uint8_t out[32]) {
 (void)unused;return PH_BL_Sha256(bytes,n,out);
}
static size_t lan_dispatch(void *unused,const ph_lan_v4_request *request,const ph_auth_context *context,
                           void *output,size_t capacity,bool *admitted) {
 (void)unused;
 return ph_command_context(&bl_device,request->action,&request->params,request->id,
                            request->receipt_key,context,PH_WIRE_MSGPACK,output,capacity,admitted);
}
#endif
bool bl_sensor_grant(const char *peer,const char *token,uint32_t boot) {
#ifdef PH_S30_LAN_V4
 return ph_lan_v4_sensor_grant(&udp_state,peer,token,boot);
#else
 (void)peer;(void)token;(void)boot;return false;
#endif
}
static __attribute__((noinline)) bool factory_reset_record(void) {
 ph_settings defaults;ph_settings_default(&defaults);
 if(!save_record(&defaults,NULL,defaults.energy_kwh,NULL,false))return false;
 memset(bl_auth.mqtt,0,sizeof(bl_auth.mqtt));
#ifdef PH_S30_LAN_V4
 ph_lan_v4_revoke(&udp_state);
#endif
 credential_owned=false;memset(committed_password,0,sizeof(committed_password));
 bl_device.settings=defaults;set_relay(NULL,false);return true;
}
static void network_thread(beken_thread_arg_t arg) {
 (void)arg;
 PH_BOOT_STAGE("lan-thread");
 for(;;) {
  uint32_t t=now(NULL);unsigned reset=0;char url[128]="";bool restart=false;
  scan_step(t);lock();
  if(!bl_ota_pending&&(int32_t)(t-reboot_at)>=0) {
   restart=reboot_pending;reset=reset_requested;reset_requested=0;
  }
  if(!reset&&!restart&&!bl_ota_pending)ph_power_poll(&bl_device);
  checkpoint(t,restart||reset==1);
  if(reset==2&&!factory_reset_record())reset=0;
  if(bl_ota_pending) {
   if(*ota_url&&(int32_t)(t-ota_at)>=0) {
    /* The shared owner delivers its captured command receipt before closing.
     * Wait for STOPPED (after buffer/socket teardown), with a bounded grace. */
    if(bl_mqtt_quiescent&&xPortGetFreeHeapSize()>=PH_OTA_START_HEAP) {
     strcpy(url,ota_url);ota_url[0]=0;
    } else if((uint32_t)(t-ota_at)>=10000) {
     ota_url[0]=0;bl_ota_pending=false;ph_device_set_maintenance(&bl_device,false);ota_failed=true;
    }
   } else if(!*ota_url&&OTA_GetProgress()<0) {
    bl_ota_pending=false;ph_device_set_maintenance(&bl_device,false);ota_failed=true;
   }
  }
  bool upgrading=bl_ota_pending;
#ifdef PH_S30_LAN_V4
  if(upgrading)ph_lan_v4_pause(&udp_state);else ph_lan_v4_poll(&udp_state);
#endif
  unlock();
  if(reset) {
   if(reset==2){CFG_SetDefaultConfig();CFG_SetMQTTUserName("");CFG_SetMQTTPass("");CFG_SetDeviceName("S30-V1B");}
   CFG_SetWiFiSSID("");CFG_SetWiFiPass("");CFG_SetWiFiSSID2("");CFG_SetWiFiPass2("");
   CFG_Save_IfThereArePendingChanges();HAL_RebootModule();
  }
  if(restart)HAL_RebootModule();
  if(upgrading) {
#ifdef PH_S30_LAN_V4
   PH_BL_MdnsPoll(false);
#endif
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
#ifdef PH_S30_LAN_V4
   if(udp_fd<0){lock();ph_lan_v4_pause(&udp_state);unlock();}
#endif
  }
#ifdef PH_S30_LAN_V4
  PH_BL_MdnsPoll(udp_fd>=0&&Main_IsConnectedToWiFi());
#endif
  fd_set reads;FD_ZERO(&reads);if(udp_fd>=0)FD_SET(udp_fd,&reads);
  struct timeval delay={0,100000};int events=select(udp_fd+1,&reads,NULL,NULL,&delay);
  if(events>0&&udp_fd>=0&&FD_ISSET(udp_fd,&reads)) {
   struct sockaddr_in remote;socklen_t size=sizeof(remote);
   int n=recvfrom(udp_fd,udp_in,sizeof(udp_in),0,(struct sockaddr*)&remote,&size);
#ifdef PH_S30_LAN_V4
   if(n>0&&n<=LAN_V4_FRAME_MAX) {
    char source[16];
    if(inet_ntop(AF_INET,&remote.sin_addr,source,sizeof(source))) {
     lock();
     /* A response is independently allocated by the adapter before dispatch.
      * Reclaim optional cache bytes before that admission, not inside it. */
     ph_status_cache_suspend(&bl_device,true);
     size_t k=ph_lan_v4_feed(&udp_state,udp_in,(size_t)n,source,ntohs(remote.sin_port),udp_in,LAN_V4_FRAME_MAX);
     unlock();if(k)sendto(udp_fd,udp_in,k,0,(struct sockaddr*)&remote,size);
    }
   }
#else
   if(n>0&&n<=PH_INPUT_MAX){lock();size_t k=ph_udp_handle(&udp_state,&bl_device,&bl_auth,udp_in,n,udp_out,sizeof(udp_out));unlock();
    /* The lab's LAN transport listens and replies on the shared service port. */
    remote.sin_port=htons(11288);if(k)sendto(udp_fd,udp_out,k,0,(struct sockaddr*)&remote,size);}
#endif
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
 uint8_t power_on=settings.power_on;
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
#ifdef PH_S30_LAN_V4
 ph_lan_v4_ops lan_ops={NULL,now,lan_free_heap,lan_digest,lan_dispatch};
 if(!ph_lan_v4_init(&udp_state,bl_device.mac,bl_device.boot,&bl_auth,&lan_ops)||
    !PH_BL_MdnsInit(PH_BL_HostName(),bl_device.mac,"full",bl_device.version))return;
#endif
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

/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Native business adapters: borrowed typed parameters, direct wire output. */
#include "service_mp.h"
static bool smp_has(const ph_value *p,const char *key){ph_value v;return phv_get(p,key,&v);}
static bool smp_int(const ph_value *p,const char *key,int *out,int lo,int hi){ph_value v;int64_t n;if(!phv_get(p,key,&v)||!phv_int(&v,&n)||n<lo||n>hi)return false;*out=(int)n;return true;}
static bool smp_bool(const ph_value *p,const char *key,bool *out){ph_value v;return phv_get(p,key,&v)&&phv_bool(&v,out);}
static bool smp_text(const ph_value *p,const char *key,char *out,size_t n,bool required){ph_value v;if(!phv_get(p,key,&v))return !required;return phv_text(&v,out,n);}
static void smp_empty(ph_writer *w){phw_begin(w,NULL,false);phw_end(w);}
void service_product_info_mp(ph_writer *w){
 phw_begin(w,NULL,false);phw_text(w,"model",DEVICE_MODEL);phw_text(w,"device_id",DEVICE_ID);
 phw_text(w,"version",DEVICE_VERSION);phw_text(w,"firmware_version",DEVICE_VERSION);
 phw_text(w,"variant",PHICOMM_FIRMWARE_VARIANT);phw_text(w,"protocol","tmcp");phw_u32(w,"protocol_version",2);
 phw_u32(w,"wire_version",1);phw_text(w,"default_wire_format","msgpack");
 phw_begin(w,"wire_formats",true);phw_text(w,NULL,"msgpack");phw_end(w);phw_end(w);
}
void service_product_status_mp(ph_writer *w){
 device_status_t snapshot;device_status_full_t full;
#ifdef FEATURE_DEVICE_A1
 a1_status_capture(&snapshot,&full);
#else
 m1_status_capture(&snapshot,&full);
#endif
 device_status_mp(w,&snapshot,&full);
}
void service_product_capabilities_mp(ph_writer *w,const char *const *extra,bool restricted){
 phw_begin(w,NULL,false);
 phw_text(w,"model",DEVICE_MODEL);phw_text(w,"device_id",DEVICE_ID);
 phw_text(w,"firmware_version",DEVICE_VERSION);phw_text(w,"variant",PHICOMM_FIRMWARE_VARIANT);
 if(!restricted){
  phw_u32(w,"protocol",2);phw_text(w,"mqtt_protocol","tmcp");phw_u32(w,"mqtt_protocol_version",2);
  phw_u32(w,"wire_version",1);phw_begin(w,"wire_formats",true);phw_text(w,NULL,"msgpack");phw_end(w);phw_text(w,"default_wire_format","msgpack");
  phw_begin(w,"status_views",true);phw_text(w,NULL,"full");phw_end(w);phw_text(w,"device_language","zh-CN,en");
  phw_u32(w,"config_backup",1);phw_bool(w,"config_recovery",device_config_recovery());
#ifndef FEATURE_DEVICE_A1
  phw_u32(w,"sensor_session_protocol",2);phw_u32(w,"sensor_schema",3);phw_u32(w,"sensor_alarm_config_schema",1);phw_u32(w,"mcu_ota_max_bytes",32768);
#else
  phw_bool(w,"schedule_countdown",true);phw_begin(w,"button_keys",true);for(unsigned i=0;svc_capability_keys[i];i++)phw_text(w,NULL,svc_capability_keys[i]);phw_end(w);
#ifdef PHICOMM_BEMFA
  phw_bool(w,"dual_mqtt",true);
#endif
#ifdef A1_FRESH_AIR
  phw_bool(w,"fresh_air",true);
#endif
#endif
 }
 phw_begin(w,"actions",true);if(!restricted)for(unsigned i=0;svc_capability_actions[i];i++)phw_text(w,NULL,svc_capability_actions[i]);
 if(extra)for(unsigned i=0;extra[i];i++)phw_text(w,NULL,extra[i]);phw_end(w);
#define STATUS_INT(k,v) phw_int(w,k,v)
#define STATUS_BOOL(k,v) phw_bool(w,k,v)
#define STATUS_STRING(k,v) phw_text(w,k,v)
#include "variant_fields.inc"
#undef STATUS_INT
#undef STATUS_BOOL
#undef STATUS_STRING
 phw_end(w);
}
#ifdef FEATURE_DEVICE_A1
static void smp_filter(ph_writer *o){
    a1_filter_storage_t f;a1_filter_get(&f);phw_begin(o,NULL,false);
    a1_filter_stock_t stock;a1_filter_stock_get(&stock);
    unsigned time_remaining=a1_filter_remaining(&f),weighted_remaining=a1_filter_stock_remaining(&stock);
    unsigned remaining=stock.weighted?weighted_remaining:time_remaining;
    phw_text(o,"model",stock.weighted?"weighted_estimate":"runtime");
    phw_int(o,"time_remaining_percent",time_remaining);
    phw_int(o,"weighted_remaining_percent",weighted_remaining);
    phw_int(o,"weighted_usage",stock.usage);
    phw_int(o,"weighted_capacity",A1_FILTER_STOCK_CAPACITY);
    phw_number(o,"air_volume_m3_estimate",stock.air_litres/1000.0);
    phw_int(o,"used_seconds",f.used_seconds);
    phw_int(o,"lifetime_hours",f.lifetime_hours);
    phw_int(o,"remaining_percent",remaining);
    phw_bool(o,"replacement_due",remaining<=10);phw_end(o);return;
}
#endif

#ifndef FEATURE_DEVICE_A1
static void smp_alarm(ph_writer *o) {
    m1_alarm_config_t a;m1_alarm_get(&a);
    mico_rtos_lock_mutex(&m1_lock);
    bool sent=m1_state.alarm_uart_sent && m1_state.alarm_sent_revision==a.revision;
    bool supported=m1_state.mcu_version_received && m1_state.mcu_alarm_schema==1;
    bool actual=supported && m1_state.alarm_buzzer_received &&
        (uint32_t)(mico_rtos_get_time()-m1_state.alarm_buzzer_updated_ms)<15000;
    unsigned mask=m1_state.alarm_buzzer_mask,schema=m1_state.mcu_alarm_schema;
    bool confirmed=actual && mask==a.reserved[1] && m1_state.alarm_buzzer_revision==a.revision;
    bool active=m1_state.alarm_buzzer_active;
    mico_rtos_unlock_mutex(&m1_lock);
    phw_begin(o,NULL,false);
    phw_int(o,"revision",a.revision);
    phw_bool(o,"hcho_configured",a.configured&1);
    phw_bool(o,"pm25_configured",a.configured&2);
    phw_int(o,"hcho_yellow",a.hcho[0]);
    phw_int(o,"hcho_red",a.hcho[1]);
    phw_int(o,"pm25_yellow",a.pm25[0]);
    phw_int(o,"pm25_red",a.pm25[1]);
    phw_bool(o,"hcho_buzzer_enabled",a.reserved[1]&1);
    phw_bool(o,"pm25_buzzer_enabled",a.reserved[1]&2);
    phw_bool(o,"buzzer_supported",supported);
    phw_int(o,"mcu_alarm_schema",schema);
    phw_bool(o,"buzzer_confirmed",confirmed);
    if(actual){
        phw_bool(o,"mcu_hcho_buzzer_enabled",mask&1);
        phw_bool(o,"mcu_pm25_buzzer_enabled",mask&2);
        phw_int(o,"mcu_buzzer_mask",mask);
        phw_bool(o,"sensor_alarm_active",active);
    }else{
        phw_null(o,"mcu_hcho_buzzer_enabled");
        phw_null(o,"mcu_pm25_buzzer_enabled");
        phw_null(o,"mcu_buzzer_mask");
        phw_null(o,"sensor_alarm_active");
    }
    phw_text(o,"buzzer_confirmation",confirmed?"mcu_readback":supported?"pending_mcu_readback":"requires_mcu_upgrade");
    phw_bool(o,"uart_sent",a.configured && sent);
    phw_text(o,"unit","main_controller_raw_u16");
    phw_text(o,"confirmation","uart_send_only");
    phw_end(o);return;
}
static void smp_wake_alarm(ph_writer *o) {
    uint32_t now=mico_rtos_get_time(),until;bool active;unsigned tone;
    mico_rtos_lock_mutex(&m1_lock);
    active=m1_state.wake_alarm_active;until=m1_state.wake_alarm_until;
    tone=m1_state.wake_alarm_tone;
    mico_rtos_unlock_mutex(&m1_lock);
    active=active && (int32_t)(until-now)>0;
    phw_begin(o,NULL,false);
    phw_bool(o,"active",active);
    phw_int(o,"remaining_seconds",active?((until-now)+999)/1000:0);
    phw_int(o,"default_duration_seconds",60);
    phw_int(o,"max_duration_seconds",180);
    phw_text(o,"stop_button_event","double_click");
    bool supported=m1_tone_supported();
    phw_bool(o,"tone_supported",supported);
    phw_int(o,"tone_count",supported?4:0);
    phw_bool(o,"music_supported",m1_music_supported());
    mico_rtos_lock_mutex(&m1_lock);bool present=m1_state.mcu_music_present;mico_rtos_unlock_mutex(&m1_lock);
    phw_bool(o,"music_present",present);
    phw_text(o,"tone_min_mcu_version","0.9.3");
    phw_int(o,"tone",tone);
    phw_end(o);return;
}
static void smp_mcu(ph_writer *o) {
    char version[17];bool received,buzzer,updating,buttons,display_source;uint32_t updated,until;unsigned protocol,caps,display_schema;
    mico_rtos_lock_mutex(&m1_lock);
    memcpy(version,m1_state.mcu_version,sizeof(version));received=m1_state.mcu_version_received;
    updated=m1_state.mcu_version_updated_ms;buzzer=m1_state.buzzer_requested;until=m1_state.buzzer_until;
    updating=m1_state.mcu_updating;protocol=m1_state.mcu_protocol;caps=m1_state.mcu_caps;
    buttons=m1_state.mcu_button_events;display_source=m1_state.mcu_display_source;
    display_schema=m1_state.mcu_display_schema;
    mico_rtos_unlock_mutex(&m1_lock);
    uint32_t now=mico_rtos_get_time();
    phw_begin(o,NULL,false);phw_text(o,"version",version);
    phw_bool(o,"version_received",received);
    phw_bool(o,"updating",updating);
    phw_int(o,"extension_protocol",protocol);
    phw_int(o,"extension_caps",caps);
    phw_bool(o,"button_events_supported",buttons);
    phw_bool(o,"display_source_supported",display_source);
    phw_int(o,"display_source_schema",display_schema);
    phw_bool(o,"dual_display_source_supported",display_schema==2);
    if(received)phw_number(o,"version_age_ms",(uint32_t)(now-updated));
    phw_bool(o,"buzzer_requested",buzzer && (int32_t)(until-now)>0);
    phw_int(o,"buzzer_remaining_ms",buzzer && (int32_t)(until-now)>0?until-now:0);
    phw_text(o,"buzzer_confirmation","command_only");phw_end(o);return;
}
#endif
extern bool feature_mp(const char *,const ph_value *,ph_writer *,const char **);
extern bool feature_ntp_set_mp(const char *,const char **);
#ifdef FEATURE_DEVICE_A1
static bool smp_control(const char *action,const ph_value *p)
{
 a1_control_t r={0};ph_value v;bool b;int number;
 bool fan=!strcmp(action,"fan.set")||!strcmp(action,"fan.mode.set");
 bool lock=!strcmp(action,"child_lock.set");r.kind=fan?A1_CONTROL_FAN:lock?A1_CONTROL_LOCK:A1_CONTROL_LIGHT;
 const char *keys=fan?"|on||speed||mode|":lock?"|enabled|":"|enabled||always_on||brightness||color|";
 if(!phv_keys(p,keys))return false;
 const char *bools[]={"on","enabled","always_on"};const unsigned flags[]={A1_SET_ON,A1_SET_ENABLED,A1_SET_ALWAYS};
 uint8_t *targets[]={&r.on,&r.enabled,&r.always_on};
 for(unsigned i=0;i<3;i++)if(phv_get(p,bools[i],&v)){if(!phv_bool(&v,&b))return false;*targets[i]=b;r.fields|=flags[i];}
 if(smp_has(p,"speed")){if(!smp_int(p,"speed",&number,0,100))return false;r.speed=number;r.fields|=A1_SET_SPEED;}
 if(smp_has(p,"brightness")){if(!smp_int(p,"brightness",&number,0,100))return false;r.brightness=(number*255+50)/100;r.fields|=A1_SET_BRIGHTNESS;}
 if(phv_get(p,"mode",&v)){if(phv_equal_text(&v,"auto"))r.mode=2;else if(phv_equal_text(&v,"manual"))r.mode=1;else return false;r.fields|=A1_SET_MODE;}
 if(phv_get(p,"color",&v)){
  if(v.kind!=PHV_MAP||!phv_keys(&v,"|r||g||b|"))return false;
  const char *colors[]={"r","g","b"};uint8_t *channels[]={&r.red,&r.green,&r.blue};
  for(unsigned i=0;i<3;i++){if(!smp_int(&v,colors[i],&number,0,255))return false;*channels[i]=number;}r.fields|=A1_SET_COLOR;
 }
 return r.fields&&a1_control_apply(&r,false,true);
}
#endif
static bool smp_config(const ph_value *p,ph_writer *w,const char **code)
{
 ph_value values;char section[16];CONFIG c;GET_CONFIG(&c);bool ok=false;int number;
 if(!phv_keys(p,"|section||values|")||!smp_text(p,"section",section,sizeof(section),true)||!phv_get(p,"values",&values)||values.kind!=PHV_MAP)goto invalid;
 if(!strcmp(section,"device")){
  uint8_t language=device_language_get();char tag[8];
  if(!phv_keys(&values,"|name||language|")||(!smp_has(&values,"name")&&!smp_has(&values,"language"))||!smp_text(&values,"name",c.name,sizeof(c.name),false))goto invalid;
  if(smp_has(&values,"language")&&(!smp_text(&values,"language",tag,sizeof(tag),true)||!ph_language_parse(tag,&language)))goto invalid;
  if(!c.name[0]){unsigned suffix=(unsigned)strtoul(DEVICE_ID+8,NULL,16);snprintf(c.name,sizeof(c.name),DEVICE_NAME_PREFIX "-%04X",suffix);}
  ok=SAVE_DEVICE_CONFIG(&c,language);
 }else if(!strcmp(section,"auth")){
  if(!phv_keys(&values,"|password|")||!smp_text(&values,"password",c.web_password,sizeof(c.web_password),true))goto invalid;
  ok=SAVE_CONFIG(&c);
 }else if(!strcmp(section,"wifi")){
  char old[sizeof(c.ssid)];memcpy(old,c.ssid,sizeof(old));
  if(!phv_keys(&values,"|ssid||password|")||!smp_text(&values,"ssid",c.ssid,sizeof(c.ssid),true)||!c.ssid[0])goto invalid;
  if(strcmp(old,c.ssid))c.wifi_password[0]=0;
  if(!smp_text(&values,"password",c.wifi_password,sizeof(c.wifi_password),false))goto invalid;
  size_t n=strlen(c.wifi_password);if(n&&(n<8||n>63))goto invalid;ok=WIFI_TRIAL(&c);
 }else if(!strcmp(section,"mqtt")){
  if(!phv_keys(&values,"|host||port||user||password||enabled|")||!smp_text(&values,"host",c.mqtt_host,sizeof(c.mqtt_host),true)||!smp_text(&values,"user",c.mqtt_user,sizeof(c.mqtt_user),true)||!smp_text(&values,"password",c.mqtt_password,sizeof(c.mqtt_password),false)||!smp_int(&values,"port",&number,1,65535))goto invalid;
  c.mqtt_port=number;
#if defined(FEATURE_DEVICE_A1) && defined(PHICOMM_BEMFA)
  bemfa_config_t next;bemfa_config_get(&next);ph_value enabled;bool active;
  if(phv_get(&values,"enabled",&enabled)){if(!phv_bool(&enabled,&active))goto invalid;next.mqtt_enabled=active;}
  next.revision++;ok=a1_mqtt_config_save(&c,&next);
#else
  if(smp_has(&values,"enabled"))goto invalid;ok=SAVE_CONFIG(&c);
#endif
 }else if(!strcmp(section,"ntp")){
  char host[64];if(!phv_keys(&values,"|ntp_host|")||!smp_text(&values,"ntp_host",host,sizeof(host),true)||!host[0])goto invalid;
  feature_ntp_set_mp(host,code);smp_empty(w);return true;
 }else if(!strcmp(section,"bemfa")){
#if defined(FEATURE_DEVICE_A1) && defined(PHICOMM_BEMFA)
  extern bool bemfa_config_set_mp(const ph_value *,const char **);
  bemfa_config_set_mp(&values,code);smp_empty(w);return true;
#else
  goto invalid;
#endif
 }else goto invalid;
 *code=ok?"accepted":"storage_error";smp_empty(w);return true;
invalid:*code="invalid_params";smp_empty(w);return true;
}
#ifndef FEATURE_DEVICE_A1
extern bool m1_display_mp(const char *,const ph_value *,ph_writer *,const char **);
extern bool m1_co2_source_mp(const char *,const ph_value *,ph_writer *,const char **);
extern bool m1_mcu_mp(const char *,const ph_value *,ph_writer *,const char **);
static bool smp_music(const char *action,const ph_value *p,ph_writer *w,const char **code)
{
 if(strncmp(action,"music.",6))return false;
 if(!m1_music_supported()){*code="unsupported";smp_empty(w);return true;}
 uint8_t f[12]={0x23,0x70,15,0,0,0,0,0,0,0,0,0x21},out[20];int n=0,offset=0;char text[11]={0};
 if(!strcmp(action,"music.get")){if(!phv_keys(p,""))goto invalid;}
 else if(!strcmp(action,"music.begin")){
  char crc[9];if(!phv_keys(p,"|length||crc32|")||!smp_int(p,"length",&n,1,1024)||!smp_text(p,"crc32",crc,sizeof(crc),true)||strlen(crc)!=8)goto invalid;
  f[4]=1;f[5]=n>>8;f[6]=n;
  for(unsigned i=0;i<8;i++){int c=crc[i],v=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;if(v<0)goto invalid;f[7+i/2]|=(uint8_t)(v<<(i%2?0:4));}
 }else if(!strcmp(action,"music.chunk")){
  if(!phv_keys(p,"|offset||data|")||!smp_int(p,"offset",&offset,0,1023)||!smp_text(p,"data",text,5,true)||!*text)goto invalid;
  f[4]=2;f[5]=offset>>8;f[6]=offset;memcpy(f+7,text,strlen(text));
 }else if(!strcmp(action,"music.commit")){if(!phv_keys(p,""))goto invalid;f[4]=3;}
 else if(!strcmp(action,"music.read")){if(!phv_keys(p,"|offset||length|")||!smp_int(p,"offset",&offset,0,1023)||!smp_int(p,"length",&n,1,10))goto invalid;f[4]=4;f[5]=offset>>8;f[6]=offset;f[7]=n;}
 else if(!strcmp(action,"music.play")){if(!phv_keys(p,""))goto invalid;f[4]=5;f[6]=180;}
 else if(!strcmp(action,"music.stop")){if(!phv_keys(p,""))goto invalid;f[4]=6;}
 else if(!strcmp(action,"music.cancel")){if(!phv_keys(p,""))goto invalid;f[4]=7;}
 else{*code="unsupported";smp_empty(w);return true;}
 if(!m1_mcu_exchange(f,12,out)){*code="busy";smp_empty(w);return true;}
 if(out[4]){*code=out[4]==1?"invalid_params":out[4]==2?"busy":out[4]==3?"unsupported":"storage_error";smp_empty(w);return true;}
 if(f[4]==4&&(out[7]!=n||out[5]!=f[5]||out[6]!=f[6])){*code="busy";smp_empty(w);return true;}
 phw_begin(w,NULL,false);
 if(!f[4]){
  phw_bool(w,"present",out[5]);phw_bool(w,"playing",out[6]);phw_u32(w,"length",(unsigned)out[7]*256+out[8]);
  char crc[9];snprintf(crc,sizeof(crc),"%02x%02x%02x%02x",out[9],out[10],out[11],out[12]);phw_text(w,"crc32",crc);
  phw_u32(w,"duration_ms",(uint32_t)out[13]<<24|(uint32_t)out[14]<<16|(uint32_t)out[15]<<8|out[16]);phw_bool(w,"uploading",out[17]);phw_u32(w,"max_length",1024);
 }else if(f[4]==4){memcpy(text,out+8,n);text[n]=0;phw_text(w,"data",text);}
 phw_end(w);*code=!f[4]||f[4]==4?"ok":"accepted";return true;
invalid:*code="invalid_params";smp_empty(w);return true;
}
bool m1_automation_execute(const automation_action_t *a,bool button)
{
 (void)button;if(!a)return false;if(!a->kind)return true;
 if(a->kind!=AUTO_DISPLAY&&a->kind!=AUTO_REBOOT&&a->kind!=AUTO_WAKE_ALARM)return false;
 if(!service_control_begin())return false;
 bool ok=a->kind==AUTO_DISPLAY?(a->value<=100&&!(a->value%25)&&m1_set_brightness(a->value/25)):
     a->kind==AUTO_REBOOT?m1_reboot_later(1500):m1_wake_alarm_tone_start(a->value,a->speed);
 service_control_end();if(ok)service_cache_changed(SERVICE_CACHE_STATUS);return ok;
}
#endif
bool service_product_mp(const char *action,const ph_value *p,ph_writer *w,const char **code)
{
 *code="invalid_params";
 if(!strcmp(action,"config.get")){
  if(!phv_keys(p,""))goto invalid;CONFIG c;GET_CONFIG(&c);phw_begin(w,NULL,false);
  phw_text(w,"migration_state",device_config_recovery()?"confirmation_required":"ready");phw_text(w,"language",ph_language_tag(device_language_get()));phw_text(w,"name",c.name);phw_text(w,"ssid",c.ssid);phw_text(w,"mqtt_host",c.mqtt_host);phw_text(w,"mqtt_user",c.mqtt_user);phw_text(w,"mqtt_protocol","tmcp");phw_u32(w,"mqtt_protocol_version",2);phw_u32(w,"mqtt_port",c.mqtt_port);
  phw_bool(w,"wifi_password_set",c.wifi_password[0]);phw_bool(w,"mqtt_password_set",c.mqtt_password[0]);phw_bool(w,"web_password_set",c.web_password[0]);
#if defined(FEATURE_DEVICE_A1) && defined(PHICOMM_BEMFA)
  extern void bemfa_config_write_mp(ph_writer *);bemfa_config_write_mp(w);
#endif
  phw_end(w);*code="ok";return true;
 }
 if(!strcmp(action,"config.set"))return smp_config(p,w,code);
 if(!strcmp(action,"wifi.scan.get")){
  if(!phv_keys(p,""))goto invalid;STATE state;GET_STATE(&state);phw_begin(w,NULL,false);phw_bool(w,"scanning",state.scan_busy);phw_begin(w,"networks",true);
  for(unsigned i=0;i<state.scan_count;i++){phw_begin(w,NULL,false);phw_text(w,"ssid",state.scan[i].ssid);phw_int(w,"rssi",state.scan[i].rssi);phw_end(w);}phw_end(w);phw_end(w);*code="ok";return true;
 }
 STATE state;GET_STATE(&state);bool query=strlen(action)>4&&!strcmp(action+strlen(action)-4,".get");
#ifndef FEATURE_DEVICE_A1
 /* Stopping an alarm is an immediate safety action, including during OTA. */
 if(!strcmp(action,"wake_alarm.get")||!strcmp(action,"wake_alarm.stop")){
  if(!phv_keys(p,""))goto invalid;if(!strcmp(action,"wake_alarm.stop"))m1_wake_alarm_stop();smp_wake_alarm(w);*code=!strcmp(action,"wake_alarm.get")?"ok":"accepted";return true;
 }
 if(!query&&state.mcu_updating)goto busy;
#endif
 if(!query&&(state.ota_active||state.reboot_pending||state.wifi_trial_active))goto busy;
#ifdef FEATURE_DEVICE_A1
 if(!strcmp(action,"filter.get")){if(!phv_keys(p,""))goto invalid;smp_filter(w);*code="ok";return true;}
 if(!strcmp(action,"filter.reset")||!strcmp(action,"filter.set")||!strcmp(action,"filter.model.set")){
  int hours;bool weighted;
  if(!strcmp(action,"filter.reset")){if(!phv_keys(p,""))goto invalid;*code=a1_filter_change(0,true);}
  else if(!strcmp(action,"filter.set")){if(!phv_keys(p,"|lifetime_hours|")||!smp_int(p,"lifetime_hours",&hours,1,A1_FILTER_MAX_HOURS))goto invalid;*code=a1_filter_change(hours,false);}
  else{if(!phv_keys(p,"|weighted|")||!smp_bool(p,"weighted",&weighted))goto invalid;*code=a1_filter_model_change(weighted);}
  if(!strcmp(*code,"accepted"))smp_filter(w);else smp_empty(w);return true;
 }
#ifdef A1_FRESH_AIR
 extern bool fresh_mp(const char *,const ph_value *,ph_writer *,const char **);
 if(fresh_mp(action,p,w,code))return true;
#endif
#else
 if(m1_display_mp(action,p,w,code)||m1_co2_source_mp(action,p,w,code)||m1_mcu_mp(action,p,w,code))return true;
 if(smp_music(action,p,w,code))return true;
 if(!strcmp(action,"alarm.get")){if(!phv_keys(p,""))goto invalid;smp_alarm(w);*code="ok";return true;}
 if(!strcmp(action,"alarm.set")){
  char sensor[8];int low,high,revision;bool buzzer;int buzzer_value=-1;
  if(!phv_keys(p,"|sensor||yellow||red||revision||buzzer_enabled|")||!smp_text(p,"sensor",sensor,sizeof(sensor),true)||(strcmp(sensor,"hcho")&&strcmp(sensor,"pm25"))||!smp_int(p,"yellow",&low,0,65535)||!smp_int(p,"red",&high,1,65535)||!smp_int(p,"revision",&revision,0,INT32_MAX)||low>high)goto invalid;
  if(smp_has(p,"buzzer_enabled")){if(!smp_bool(p,"buzzer_enabled",&buzzer))goto invalid;buzzer_value=buzzer;}
  *code=m1_alarm_set(!strcmp(sensor,"pm25"),low,high,revision,buzzer_value);if(!strcmp(*code,"accepted"))smp_alarm(w);else smp_empty(w);return true;
 }
 if(!strcmp(action,"wake_alarm.start")){
  int seconds=60,tone=0;if(!phv_keys(p,"|duration_seconds||tone|")||(smp_has(p,"duration_seconds")&&!smp_int(p,"duration_seconds",&seconds,1,180))||(smp_has(p,"tone")&&!smp_int(p,"tone",&tone,0,5)))goto invalid;
  if((tone&&!m1_tone_supported())||(tone==5&&!m1_music_supported())){*code="unsupported";smp_empty(w);return true;}
  *code=m1_wake_alarm_tone_start(seconds,tone)?"accepted":"busy";if(!strcmp(*code,"accepted"))smp_wake_alarm(w);else smp_empty(w);return true;
 }
 if(!strcmp(action,"mcu.version.get")){if(!phv_keys(p,""))goto invalid;smp_mcu(w);*code="ok";return true;}
#endif
 if(feature_mp(action,p,w,code))return true;
 bool ok=false,value;
#ifndef FEATURE_DEVICE_A1
 int number;
#endif
 if(!strcmp(action,"system.reboot")){if(!phv_keys(p,""))goto invalid;ok=REBOOT(1500);}
 else if(!strcmp(action,"system.reset")){if(!phv_keys(p,"|full|")||!smp_bool(p,"full",&value))goto invalid;ok=RESET(value);}
 else if(!strcmp(action,"wifi.scan")){if(!phv_keys(p,""))goto invalid;ok=SCAN();}
 else if(!strcmp(action,"ota.start")){char url[512];if(!phv_keys(p,"|url|")||!smp_text(p,"url",url,sizeof(url),true))goto invalid;ok=DOWNLOAD(url);}
#ifdef FEATURE_DEVICE_A1
 else if(!strcmp(action,"fan.set")||!strcmp(action,"fan.mode.set")||!strcmp(action,"light.set")||!strcmp(action,"child_lock.set"))ok=smp_control(action,p);
 else if(!strcmp(action,"fan.panel")){if(!phv_keys(p,""))goto invalid;ok=a1_panel_inspect();}
 else if(!strcmp(action,"fan.mode.toggle")){if(!phv_keys(p,""))goto invalid;a1_control_t r={.kind=A1_CONTROL_FAN,.fields=A1_SET_MODE,.mode=3};ok=a1_control_apply(&r,false,true);}
 else if(!strcmp(action,"light_always.set")){if(!phv_keys(p,"|enabled|")||!smp_bool(p,"enabled",&value))goto invalid;a1_control_t r={.kind=A1_CONTROL_LIGHT,.fields=A1_SET_ALWAYS,.always_on=value};ok=a1_control_apply(&r,false,true);}
#else
 else if(!strcmp(action,"display.set")){if(!phv_keys(p,"|brightness|")||!smp_int(p,"brightness",&number,0,100)||number%25)goto invalid;ok=m1_set_brightness(number/25);}
 else if(!strcmp(action,"mcu.version.query")){if(!phv_keys(p,""))goto invalid;ok=m1_uart_version_query();}
 else if(!strcmp(action,"buzzer.set")){int duration=500;if(!phv_keys(p,"|enabled||duration_ms|")||!smp_bool(p,"enabled",&value)||(smp_has(p,"duration_ms")&&!smp_int(p,"duration_ms",&duration,1,5000)))goto invalid;ok=m1_uart_buzzer(value,duration);}
#endif
 else return false;
 *code=ok?"accepted":"busy";smp_empty(w);return true;
invalid:*code="invalid_params";smp_empty(w);return true;
busy:*code="busy";smp_empty(w);return true;
}

bool service_product_ota_start(const char *url){return DOWNLOAD(url);}

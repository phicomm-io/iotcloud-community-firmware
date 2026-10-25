/* Shared M1/A1 adapter, compiled once per firmware. */
#include "service.h"
#include "status_view.h"
#ifdef FEATURE_DEVICE_A1
#include "a1.h"
#include "filter.h"
#ifdef A1_FRESH_AIR
#include "fresh_air.h"
#endif
#define DEVICE_MODEL "a1"
#define CONFIG a1_config_t
#define STATE a1_state_t
#define GET_CONFIG a1_config_get
#define GET_STATE a1_state_get
#define SAVE_CONFIG a1_config_save
#define SAVE_DEVICE_CONFIG a1_device_config_save
#define STATUS a1_status_json
#define STATUS_LIVE a1_status_live_json
#define REBOOT a1_reboot_later
#define RESET a1_reset
#define WIFI_TRIAL a1_wifi_trial
#define SCAN a1_scan_start
#define DOWNLOAD a1_ota_download
#define DEVICE_NAME_PREFIX "A1"
#define DEVICE_ID a1_id
#define DEVICE_VERSION A1_VERSION
#else
#include "m1.h"
#define DEVICE_MODEL "m1"
#define CONFIG m1_config_t
#define STATE m1_state_t
#define GET_CONFIG m1_config_get
#define GET_STATE m1_state_get
#define SAVE_CONFIG m1_config_save
#define SAVE_DEVICE_CONFIG m1_device_config_save
#define STATUS m1_status_json
#define STATUS_LIVE m1_status_live_json
#define REBOOT m1_reboot_later
#define RESET m1_reset
#define WIFI_TRIAL m1_wifi_trial
#define SCAN m1_scan_start
#define DOWNLOAD m1_ota_download
#define DEVICE_NAME_PREFIX "M1"
#define DEVICE_ID m1_id
#define DEVICE_VERSION M1_VERSION
#endif
#include "device_status.h"
#if !defined(HOST_A1) && !defined(HOST_M1)
/* POSIX simulator provides its existing peripheral/status boundary. */
__attribute__((noinline)) struct printbuf *service_product_status_wire(bool live,bool envelope,const char *id)
{
    device_status_t snapshot;
    device_status_full_t full;
#ifdef FEATURE_DEVICE_A1
    a1_status_capture(&snapshot,live?NULL:&full);
#else
    m1_status_capture(&snapshot,live?NULL:&full);
#endif
    return device_status_wire(&snapshot,live?NULL:&full,envelope,id);
}
#endif
static void svc_string(json_object *o,const char *key,const char *value){json_build_object_add(o,key,json_object_new_string(value));}
static bool svc_keys(json_object *p,const char *allowed){
    json_object_object_foreach(p,key,value){char needle[80];snprintf(needle,sizeof(needle),"|%s|",key);if(!value || !strstr(allowed,needle))return false;}return true;
}
static bool svc_int(json_object *p,const char *key,int *out,int lo,int hi){json_object *v=json_object_object_get(p,key);if(!v || !json_object_is_type(v,json_type_int))return false;*out=json_object_get_int(v);return *out>=lo && *out<=hi;}
static bool svc_bool(json_object *p,const char *key,bool *out){json_object *v=json_object_object_get(p,key);if(!v || !json_object_is_type(v,json_type_boolean))return false;*out=json_object_get_boolean(v);return true;}
static bool svc_text(json_object *p,const char *key,char *out,size_t capacity,bool required){json_object *v=json_object_object_get(p,key);if(!v)return !required;if(!json_object_is_type(v,json_type_string))return false;size_t n=json_object_get_string_len(v);if(n>=capacity || n!=strlen(json_object_get_string(v)))return false;memcpy(out,json_object_get_string(v),n+1);return true;}
json_object *service_product_capabilities(void){
    json_object *o=json_object_new_object(),*a=json_object_new_array();svc_string(o,"model",DEVICE_MODEL);json_build_object_add(o,"protocol",json_object_new_int(2));svc_string(o,"mqtt_protocol","tmcp");json_build_object_add(o,"mqtt_protocol_version",json_object_new_int(2));
    status_view_capabilities(o);
    svc_string(o,"device_language","zh-CN,en");json_build_object_add(o,"config_backup",json_object_new_int(1));json_build_object_add(o,"config_recovery",json_object_new_boolean(device_config_recovery()));
#ifndef FEATURE_DEVICE_A1
    json_build_object_add(o,"sensor_session_protocol",json_object_new_int(2));
    json_build_object_add(o,"sensor_schema",json_object_new_int(1));
    json_build_object_add(o,"sensor_alarm_config_schema",json_object_new_int(1));
#endif
    const char *actions[]={"schedules.get","schedules.put","schedules.delete","schedules.enable","buttons.get","status.get","capabilities.get","config.get","logs.get","config.set","wifi.scan","wifi.scan.get","binding.status.get","system.reboot","system.reset","ota.start","night.get","night.set","time.get","time.set","binding.get",
#ifdef FEATURE_DEVICE_A1
    "binding.set",
#endif

#ifdef FEATURE_DEVICE_A1
#ifdef A1_FRESH_AIR
    "fresh_air.set","fresh_air.config.get","fresh_air.config.set","fresh_air.linkage.get","fresh_air.linkage.set",
#endif
    "filter.model.set","filter.get","filter.set","filter.reset","light_effect.next","light_effect.select","light_always.set","sensors.average.get","buttons.set","binding.probe","light_effect.get","light_effect.set","fan.set","fan.mode.set","fan.mode.toggle","fan.panel","light.set","child_lock.set","linkage.get","linkage.set",
#else
    "sensor_details.get","display.set","buzzer.set","mcu.version.get","mcu.version.query","alarm.get","alarm.set","co2_display.get","co2_display.set","co2_source.get","co2_source.set",
    "display_source.get","display_source.set","display_data.set",
    "wake_alarm.get","wake_alarm.start","wake_alarm.stop",
    "music.get","music.begin","music.chunk","music.commit","music.read","music.play","music.stop","music.cancel",
#endif
    NULL};for(unsigned i=0;actions[i];i++)json_build_array_add(a,json_object_new_string(actions[i]));json_build_object_add(o,"actions",a);
#ifdef FEATURE_DEVICE_A1
#ifdef PHICOMM_BEMFA
    json_build_object_add(o,"dual_mqtt",json_object_new_boolean(true));
#endif
    json_build_object_add(o,"schedule_countdown",json_object_new_boolean(true));
    json_object *keys=json_object_new_array();
    const char *names[]={"power","wifi","combination"};
    for(unsigned i=0;i<3;i++)json_build_array_add(keys,json_object_new_string(names[i]));
    json_build_object_add(o,"button_keys",keys);
#endif
#ifdef A1_FRESH_AIR
    json_build_object_add(o,"fresh_air",json_object_new_boolean(true));
#endif
    return o;
}
static __attribute__((noinline)) json_object *service_product_inner(const char *action,json_object *p,const char **code){
    *code="invalid_params";
    if(!strcmp(action,"config.get")){
        if(!svc_keys(p,""))return NULL;CONFIG c;GET_CONFIG(&c);json_object *o=json_object_new_object();
        svc_string(o,"migration_state",device_config_recovery()?"confirmation_required":"ready");svc_string(o,"language",ph_language_tag(device_language_get()));svc_string(o,"name",c.name);svc_string(o,"ssid",c.ssid);svc_string(o,"mqtt_host",c.mqtt_host);svc_string(o,"mqtt_user",c.mqtt_user);svc_string(o,"mqtt_protocol","tmcp");json_build_object_add(o,"mqtt_protocol_version",json_object_new_int(2));json_build_object_add(o,"mqtt_port",json_object_new_int(c.mqtt_port));
        json_build_object_add(o,"wifi_password_set",json_object_new_boolean(c.wifi_password[0]));json_build_object_add(o,"mqtt_password_set",json_object_new_boolean(c.mqtt_password[0]));json_build_object_add(o,"web_password_set",json_object_new_boolean(c.web_password[0]));
#if defined(FEATURE_DEVICE_A1) && defined(PHICOMM_BEMFA)
        bemfa_config_encode(o);
#endif
        *code="ok";return o;
    }
#ifndef FEATURE_DEVICE_A1
    if(!strcmp(action,"sensor_details.get")){if(!svc_keys(p,""))return NULL;*code="ok";return m1_sensor_details_json();}
#endif
    STATE state;GET_STATE(&state);
    if(!strcmp(action,"wifi.scan.get")){
        if(!svc_keys(p,""))return NULL;json_object *o=json_object_new_object(),*list=json_object_new_array();
        json_build_object_add(o,"scanning",json_object_new_boolean(state.scan_busy));
        for(unsigned i=0;i<state.scan_count;i++){json_object *ap=json_object_new_object();svc_string(ap,"ssid",state.scan[i].ssid);json_build_object_add(ap,"rssi",json_object_new_int(state.scan[i].rssi));json_build_array_add(list,ap);}
        json_build_object_add(o,"networks",list);*code="ok";return o;
    }
    if(!strcmp(action,"binding.status.get")){if(!svc_keys(p,""))return NULL;*code="ok";return feature_json("status");}
#ifdef FEATURE_DEVICE_A1
    if(!strcmp(action,"filter.get")){if(!svc_keys(p,""))return NULL;*code="ok";return a1_filter_json();}
    if(!strcmp(action,"sensors.average.get")){if(!svc_keys(p,""))return NULL;*code="ok";return feature_json("average");}
    if(!strcmp(action,"binding.probe")){json_object *out=NULL;int rc=feature_set("probe",p,&out);*code=rc<300?"accepted":"invalid_params";return out;}
#endif
#ifndef FEATURE_DEVICE_A1
    if(!strcmp(action,"co2_display.get")) {
        if(!svc_keys(p,""))return NULL;*code="ok";return m1_co2_json();
    }
    if(!strcmp(action,"co2_source.get")){if(!svc_keys(p,""))return NULL;*code="ok";return m1_co2_source_json();}
    if(!strcmp(action,"display_source.get")){if(!svc_keys(p,""))return NULL;*code="ok";return m1_display_source_json();}
    if(!strcmp(action,"alarm.get")) {
        if(!svc_keys(p,""))return NULL;*code="ok";return m1_alarm_json();
    }
    if(!strcmp(action,"wake_alarm.get")) {
        if(!svc_keys(p,""))return NULL;*code="ok";return m1_wake_alarm_json();
    }
    if(!strcmp(action,"wake_alarm.stop")) {
        if(!svc_keys(p,""))return NULL;m1_wake_alarm_stop();*code="accepted";return m1_wake_alarm_json();
    }
    if(!strcmp(action,"mcu.version.get")) {
        if(!svc_keys(p,""))return NULL;*code="ok";
        return m1_mcu_json();
    }
#endif
#ifndef FEATURE_DEVICE_A1
    if(state.mcu_updating && (strlen(action)<4 || strcmp(action+strlen(action)-4,".get"))){*code="busy";return NULL;}
#endif
    bool query=strlen(action)>4 && !strcmp(action+strlen(action)-4,".get");
    if(!query && (state.ota_active || state.reboot_pending || state.wifi_trial_active)){*code="busy";return NULL;}
#ifdef FEATURE_DEVICE_A1
#ifdef A1_FRESH_AIR
    if(!strncmp(action,"fresh_air.",10))return fresh_command(action,p,code);
#endif
    if(!strcmp(action,"filter.model.set")) {
        bool weighted;if(!svc_keys(p,"|weighted|") || !svc_bool(p,"weighted",&weighted))return NULL;
        *code=a1_filter_model_change(weighted);return !strcmp(*code,"accepted")?a1_filter_json():NULL;
    }
    if(!strcmp(action,"filter.reset")){
        if(!svc_keys(p,""))return NULL;
        *code=a1_filter_change(0,true);return !strcmp(*code,"accepted")?a1_filter_json():NULL;
    }
    if(!strcmp(action,"filter.set")){
        int hours;if(!svc_keys(p,"|lifetime_hours|") || !svc_int(p,"lifetime_hours",&hours,1,A1_FILTER_MAX_HOURS))return NULL;
        *code=a1_filter_change(hours,false);return !strcmp(*code,"accepted")?a1_filter_json():NULL;
    }
#endif
#ifndef FEATURE_DEVICE_A1
    if(!strcmp(action,"co2_display.set"))return m1_co2_set(p,code);
    if(!strcmp(action,"co2_source.set"))return m1_co2_source_set(p,code);
    if(!strcmp(action,"display_source.set"))return m1_display_source_set(p,code);
    if(!strcmp(action,"display_data.set"))return m1_display_data_set(p,code);
    if(!strcmp(action,"alarm.set")) {
        char sensor[8];int low,high,revision;
        json_object *buzzer=json_object_object_get(p,"buzzer_enabled");
        if(!svc_keys(p,"|sensor||yellow||red||revision||buzzer_enabled|") ||
           (buzzer && !json_object_is_type(buzzer,json_type_boolean)) ||
           !svc_text(p,"sensor",sensor,sizeof(sensor),true) ||
           (strcmp(sensor,"hcho") && strcmp(sensor,"pm25")) ||
           !svc_int(p,"yellow",&low,0,65535) || !svc_int(p,"red",&high,1,65535) ||
           !svc_int(p,"revision",&revision,0,INT32_MAX) || low>high)return NULL;
        *code=m1_alarm_set(!strcmp(sensor,"pm25"),low,high,revision,buzzer?json_object_get_boolean(buzzer):-1);
        return !strcmp(*code,"accepted")?m1_alarm_json():NULL;
    }
    if(!strncmp(action,"music.",6)) {
        if(!m1_music_supported()){*code="unsupported";return NULL;}
        uint8_t f[12]={0x23,0x70,15,0,0,0,0,0,0,0,0,0x21},out[20];
        int n=0,offset=0;char text[11]={0};
        if(!strcmp(action,"music.get")){if(!svc_keys(p,"|"))return NULL;}
        else if(!strcmp(action,"music.begin")){
            char crc[9];if(!svc_keys(p,"|length||crc32|")||!svc_int(p,"length",&n,1,1024)||!svc_text(p,"crc32",crc,sizeof(crc),true)||strlen(crc)!=8)return NULL;
            f[4]=1;f[5]=n>>8;f[6]=n;
            for(unsigned i=0;i<8;i++){
                int c=crc[i],v=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
                if(v<0)return NULL;f[7+i/2]|=(uint8_t)(v<<(i%2?0:4));
            }
        }else if(!strcmp(action,"music.chunk")){
            if(!svc_keys(p,"|offset||data|")||!svc_int(p,"offset",&offset,0,1023)||!svc_text(p,"data",text,5,true)||!*text)return NULL;
            f[4]=2;f[5]=offset>>8;f[6]=offset;memcpy(f+7,text,strlen(text));
        }else if(!strcmp(action,"music.commit")){if(!svc_keys(p,"|"))return NULL;f[4]=3;}
        else if(!strcmp(action,"music.read")){
            if(!svc_keys(p,"|offset||length|")||!svc_int(p,"offset",&offset,0,1023)||!svc_int(p,"length",&n,1,10))return NULL;
            f[4]=4;f[5]=offset>>8;f[6]=offset;f[7]=n;
        }else if(!strcmp(action,"music.play")){
            if(!svc_keys(p,"|"))return NULL;f[4]=5;f[6]=180;
        }else if(!strcmp(action,"music.stop")){if(!svc_keys(p,"|"))return NULL;f[4]=6;}
        else if(!strcmp(action,"music.cancel")){if(!svc_keys(p,"|"))return NULL;f[4]=7;}
        else {*code="unsupported";return NULL;}
        if(!m1_mcu_exchange(f,12,out)){*code="busy";return NULL;}
        if(out[4]){*code=out[4]==1?"invalid_params":out[4]==2?"busy":out[4]==3?"unsupported":"storage_error";return NULL;}
        json_object *o=json_object_new_object();
        if(f[4]==0){
            json_build_object_add(o,"present",json_object_new_boolean(out[5]));
            json_build_object_add(o,"playing",json_object_new_boolean(out[6]));
            json_build_object_add(o,"length",json_object_new_int((unsigned)out[7]*256+out[8]));
            char crc[9];snprintf(crc,sizeof(crc),"%02x%02x%02x%02x",out[9],out[10],out[11],out[12]);svc_string(o,"crc32",crc);
            json_build_object_add(o,"duration_ms",json_object_new_int((uint32_t)out[13]<<24|(uint32_t)out[14]<<16|(uint32_t)out[15]<<8|out[16]));
            json_build_object_add(o,"uploading",json_object_new_boolean(out[17]));
            json_build_object_add(o,"max_length",json_object_new_int(1024));
        }else if(f[4]==4){
            if(out[7]!=n || out[5]!=f[5] || out[6]!=f[6]){*code="busy";json_object_put(o);return NULL;}
            memcpy(text,out+8,n);text[n]=0;svc_string(o,"data",text);
        }
        *code=f[4]==0||f[4]==4?"ok":"accepted";return o;
    }
    if(!strcmp(action,"wake_alarm.start")) {
        int seconds=60,tone=0;
        if(!svc_keys(p,"|duration_seconds||tone|") || (json_object_object_get(p,"duration_seconds") &&
           !svc_int(p,"duration_seconds",&seconds,1,180)) || (json_object_object_get(p,"tone") &&
           !svc_int(p,"tone",&tone,0,5)))return NULL;
        if((tone && !m1_tone_supported()) || (tone==5 && !m1_music_supported())){*code="unsupported";return NULL;}
        *code=m1_wake_alarm_tone_start(seconds,tone)?"accepted":"busy";
        return !strcmp(*code,"accepted")?m1_wake_alarm_json():NULL;
    }
    if(!strcmp(action,"mcu.version.query")) {
        if(!svc_keys(p,""))return NULL;*code=m1_uart_version_query()?"accepted":"busy";return NULL;
    }
    if(!strcmp(action,"buzzer.set")) {
        bool enabled;int duration=500;
        if(!svc_keys(p,"|enabled||duration_ms|") || !svc_bool(p,"enabled",&enabled) ||
           (json_object_object_get(p,"duration_ms") && !svc_int(p,"duration_ms",&duration,1,5000)))return NULL;
        *code=m1_uart_buzzer(enabled,duration)?"accepted":"busy";return NULL;
    }
#endif
    const char *domains[]={"binding","night","time","linkage","light_effect",NULL};
    const char *native[]={"binding","sleep","time","linkage","light",NULL};
    for(unsigned i=0;domains[i];i++){
#ifndef FEATURE_DEVICE_A1
        if(i>=3)break;
#endif
        char key[48];snprintf(key,sizeof(key),"%s.get",domains[i]);
        if(!strcmp(action,key)){if(!svc_keys(p,""))return NULL;*code="ok";return feature_json(native[i]);}
        snprintf(key,sizeof(key),"%s.set",domains[i]);if(!strcmp(action,key)){json_object *out=NULL;int rc=feature_set(native[i],p,&out);*code=rc<300?"accepted":rc==409?"busy":rc>=500?"storage_error":"invalid_params";return out;}
    }
    bool ok=false,value=false;int number=0;
    if(!strcmp(action,"system.reboot")){if(!svc_keys(p,""))return NULL;ok=REBOOT(1500);}
    else if(!strcmp(action,"system.reset")){if(!svc_keys(p,"|full|") || !svc_bool(p,"full",&value))return NULL;ok=RESET(value);}
    else if(!strcmp(action,"wifi.scan")){if(!svc_keys(p,""))return NULL;ok=SCAN();}
    else if(!strcmp(action,"ota.start")){char url[512];if(!svc_keys(p,"|url|") || !svc_text(p,"url",url,sizeof(url),true))return NULL;ok=DOWNLOAD(url);}
#ifdef FEATURE_DEVICE_A1
    else if(!strcmp(action,"light_effect.next") || !strcmp(action,"light_effect.select")){
        bool next=!strcmp(action,"light_effect.next");
        if(next?!svc_keys(p,""):(!svc_keys(p,"|effect|") || !svc_int(p,"effect",&number,0,5)))return NULL;
        json_object *out=NULL;int rc=feature_light_select(next?255:number,&out);
        *code=rc<300?"accepted":rc==409?"busy":rc>=500?"storage_error":"invalid_params";return out;
    }
    else if(!strcmp(action,"light_always.set")){
        if(!svc_keys(p,"|enabled|") || !svc_bool(p,"enabled",&value))return NULL;
        a1_control_t r={.kind=A1_CONTROL_LIGHT,.fields=A1_SET_ALWAYS,.always_on=value};
        ok=a1_control_apply(&r,false,true);
    }
    else if(!strcmp(action,"fan.mode.set") || !strcmp(action,"fan.mode.toggle")) {
        bool toggle=!strcmp(action,"fan.mode.toggle");
        if(!svc_keys(p,toggle?"":"|mode|"))return NULL;
        if(toggle){
            a1_control_t r={.kind=A1_CONTROL_FAN,.fields=A1_SET_MODE,.mode=3};
            ok=a1_control_apply(&r,false,true);
        }else{
            json_object *mode=json_object_object_get(p,"mode");
            if(!mode || !json_object_is_type(mode,json_type_string))return NULL;
            ok=a1_control_json("fan",p);
        }
    }
    else if(!strcmp(action,"fan.panel")){
        if(!svc_keys(p,""))return NULL;
        ok=a1_panel_inspect();
    }
    else if(!strcmp(action,"fan.set")){if(!svc_keys(p,"|on||speed||mode|"))return NULL;if(json_object_object_get(p,"on")&&!svc_bool(p,"on",&value))return NULL;if(json_object_object_get(p,"speed")&&!svc_int(p,"speed",&number,0,100))return NULL;ok=a1_control_json("fan",p);}
    else if(!strcmp(action,"child_lock.set")){if(!svc_keys(p,"|enabled|") || !svc_bool(p,"enabled",&value))return NULL;ok=a1_control_json("child_lock",p);}
    else if(!strcmp(action,"light.set")){
        if(!svc_keys(p,"|enabled||always_on||brightness||color|"))return NULL;
        if(json_object_object_get(p,"brightness") && !svc_int(p,"brightness",&number,0,100))return NULL;
        ok=a1_control_light_percent(p);
    }
#else
    else if(!strcmp(action,"display.set")){if(!svc_keys(p,"|brightness|") || !svc_int(p,"brightness",&number,0,100) || number%25)return NULL;ok=m1_set_brightness(number/25);}
#endif
    else if(!strcmp(action,"config.set")){
        json_object *section=json_object_object_get(p,"section"),*values=json_object_object_get(p,"values");
        if(!svc_keys(p,"|section||values|") || !section || !json_object_is_type(section,json_type_string) || !values || !json_object_is_type(values,json_type_object))return NULL;
        const char *s=json_object_get_string(section);CONFIG c;GET_CONFIG(&c);
        if(!strcmp(s,"device")){
            uint8_t language=device_language_get();char tag[8];
            if(!svc_keys(values,"|name||language|") || (!json_object_object_get(values,"name") && !json_object_object_get(values,"language")) || !svc_text(values,"name",c.name,sizeof(c.name),false))return NULL;
            if(json_object_object_get(values,"language") && (!svc_text(values,"language",tag,sizeof(tag),true) || !ph_language_parse(tag,&language)))return NULL;
            if(!c.name[0]){unsigned suffix=(unsigned)strtoul(DEVICE_ID+8,NULL,16);snprintf(c.name,sizeof(c.name),DEVICE_NAME_PREFIX "-%04X",suffix);}
            ok=SAVE_DEVICE_CONFIG(&c,language);
        }
        else if(!strcmp(s,"auth")){if(!svc_keys(values,"|password|") || !svc_text(values,"password",c.web_password,sizeof(c.web_password),true))return NULL;ok=SAVE_CONFIG(&c);}
        else if(!strcmp(s,"bemfa")){
#if defined(FEATURE_DEVICE_A1) && defined(PHICOMM_BEMFA)
            bemfa_config_t next;bemfa_config_get(&next);
            if(!bemfa_config_values(&next,values,false))return NULL;
            ok=bemfa_config_save(&next);
#else
            return NULL;
#endif
        }
        else if(!strcmp(s,"mqtt")){
#if defined(FEATURE_DEVICE_A1) && defined(PHICOMM_BEMFA)
            bemfa_config_t next;bemfa_config_get(&next);bool enabled=next.mqtt_enabled;
            if(json_object_object_get(values,"enabled") && !svc_bool(values,"enabled",&enabled))return NULL;
            next.mqtt_enabled=enabled;next.revision++;
#endif
            if(!svc_keys(values,"|host||port||user||password||enabled|") || !svc_text(values,"host",c.mqtt_host,sizeof(c.mqtt_host),true) || !svc_text(values,"user",c.mqtt_user,sizeof(c.mqtt_user),true) || !svc_text(values,"password",c.mqtt_password,sizeof(c.mqtt_password),false) || !svc_int(values,"port",&number,1,65535))return NULL;
            c.mqtt_port=number;
#if defined(FEATURE_DEVICE_A1) && defined(PHICOMM_BEMFA)
            ok=a1_mqtt_config_save(&c,&next);
#else
            if(json_object_object_get(values,"enabled"))return NULL;
            ok=SAVE_CONFIG(&c);
#endif
        }else if(!strcmp(s,"ntp")){
            char host[64];if(!svc_keys(values,"|ntp_host|") || !svc_text(values,"ntp_host",host,sizeof(host),true) || !host[0])return NULL;
            json_object *time=feature_json("time");if(!time){*code="busy";return NULL;}svc_string(time,"ntp_host",host);json_object *out=NULL;int rc=feature_set("time",time,&out);json_object_put(time);*code=rc<300?"accepted":"storage_error";return out;
        }else if(!strcmp(s,"wifi")){
            char old[33];strcpy(old,c.ssid);
            if(!svc_keys(values,"|ssid||password|") || !svc_text(values,"ssid",c.ssid,sizeof(c.ssid),true) || !c.ssid[0])return NULL;
            if(strcmp(old,c.ssid))c.wifi_password[0]=0;
            if(!svc_text(values,"password",c.wifi_password,sizeof(c.wifi_password),false))return NULL;size_t n=strlen(c.wifi_password);if(n && (n<8 || n>63))return NULL;ok=WIFI_TRIAL(&c);
        }else return NULL;
        *code=ok?"accepted":"storage_error";return NULL;
    }else{*code="unsupported";return NULL;}
    *code=ok?"accepted":"busy";return NULL;
}

/* Status snapshots are also published inside the MQTT owner. Do not reserve the
 * configuration/write handler's large frame on this frequent nested path. */
json_object *service_product(const char *action,json_object *p,const char **code){
    if(!strcmp(action,"info.get")){
        bool live;*code="invalid_params";
        if(!status_view_params(p,&live))return NULL;
        /* Identity is immutable after startup. Avoid capturing and parsing a
         * complete status graph for these three metadata fields. */
        json_object *o=json_object_new_object();
        svc_string(o,"model",DEVICE_MODEL);svc_string(o,"device_id",DEVICE_ID);svc_string(o,"version",DEVICE_VERSION);
        *code=o?"ok":"busy";return o;
    }
    if(!strcmp(action,"status.get")){
        *code="invalid_params";
        bool live;
        if(!status_view_params(p,&live))return NULL;
        *code="ok";return live?STATUS_LIVE():STATUS();
    }
    return service_product_inner(action,p,code);
}

#ifdef FEATURE_DEVICE_A1
json_object *a1_filter_json(void){
    a1_filter_storage_t f;a1_filter_get(&f);json_object *o=json_object_new_object();
    a1_filter_stock_t stock;a1_filter_stock_get(&stock);
    unsigned time_remaining=a1_filter_remaining(&f),weighted_remaining=a1_filter_stock_remaining(&stock);
    unsigned remaining=stock.weighted?weighted_remaining:time_remaining;
    svc_string(o,"model",stock.weighted?"weighted_estimate":"runtime");
    json_build_object_add(o,"time_remaining_percent",json_object_new_int(time_remaining));
    json_build_object_add(o,"weighted_remaining_percent",json_object_new_int(weighted_remaining));
    json_build_object_add(o,"weighted_usage",json_object_new_int(stock.usage));
    json_build_object_add(o,"weighted_capacity",json_object_new_int(A1_FILTER_STOCK_CAPACITY));
    json_build_object_add(o,"air_volume_m3_estimate",json_object_new_double(stock.air_litres/1000.0));
    json_build_object_add(o,"used_seconds",json_object_new_int(f.used_seconds));
    json_build_object_add(o,"lifetime_hours",json_object_new_int(f.lifetime_hours));
    json_build_object_add(o,"remaining_percent",json_object_new_int(remaining));
    json_build_object_add(o,"replacement_due",json_object_new_boolean(remaining<=10));return o;
}
#endif

#ifndef FEATURE_DEVICE_A1
json_object *m1_alarm_json(void) {
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
    json_object *o=json_object_new_object();
    json_build_object_add(o,"revision",json_object_new_int(a.revision));
    json_build_object_add(o,"hcho_configured",json_object_new_boolean(a.configured&1));
    json_build_object_add(o,"pm25_configured",json_object_new_boolean(a.configured&2));
    json_build_object_add(o,"hcho_yellow",json_object_new_int(a.hcho[0]));
    json_build_object_add(o,"hcho_red",json_object_new_int(a.hcho[1]));
    json_build_object_add(o,"pm25_yellow",json_object_new_int(a.pm25[0]));
    json_build_object_add(o,"pm25_red",json_object_new_int(a.pm25[1]));
    json_build_object_add(o,"hcho_buzzer_enabled",json_object_new_boolean(a.reserved[1]&1));
    json_build_object_add(o,"pm25_buzzer_enabled",json_object_new_boolean(a.reserved[1]&2));
    json_build_object_add(o,"buzzer_supported",json_object_new_boolean(supported));
    json_build_object_add(o,"mcu_alarm_schema",json_object_new_int(schema));
    json_build_object_add(o,"buzzer_confirmed",json_object_new_boolean(confirmed));
    if(actual){
        json_build_object_add(o,"mcu_hcho_buzzer_enabled",json_object_new_boolean(mask&1));
        json_build_object_add(o,"mcu_pm25_buzzer_enabled",json_object_new_boolean(mask&2));
        json_build_object_add(o,"mcu_buzzer_mask",json_object_new_int(mask));
        json_build_object_add(o,"sensor_alarm_active",json_object_new_boolean(active));
    }else{
        json_object_object_add(o,"mcu_hcho_buzzer_enabled",NULL);
        json_object_object_add(o,"mcu_pm25_buzzer_enabled",NULL);
        json_object_object_add(o,"mcu_buzzer_mask",NULL);
        json_object_object_add(o,"sensor_alarm_active",NULL);
    }
    svc_string(o,"buzzer_confirmation",confirmed?"mcu_readback":supported?"pending_mcu_readback":"requires_mcu_upgrade");
    json_build_object_add(o,"uart_sent",json_object_new_boolean(a.configured && sent));
    svc_string(o,"unit","main_controller_raw_u16");
    svc_string(o,"confirmation","uart_send_only");
    return o;
}
json_object *m1_wake_alarm_json(void) {
    uint32_t now=mico_rtos_get_time(),until;bool active;unsigned tone;
    mico_rtos_lock_mutex(&m1_lock);
    active=m1_state.wake_alarm_active;until=m1_state.wake_alarm_until;
    tone=m1_state.wake_alarm_tone;
    mico_rtos_unlock_mutex(&m1_lock);
    active=active && (int32_t)(until-now)>0;
    json_object *o=json_object_new_object();
    json_build_object_add(o,"active",json_object_new_boolean(active));
    json_build_object_add(o,"remaining_seconds",json_object_new_int(active?((until-now)+999)/1000:0));
    json_build_object_add(o,"default_duration_seconds",json_object_new_int(60));
    json_build_object_add(o,"max_duration_seconds",json_object_new_int(180));
    svc_string(o,"stop_button_event","double_click");
    bool supported=m1_tone_supported();
    json_build_object_add(o,"tone_supported",json_object_new_boolean(supported));
    json_build_object_add(o,"tone_count",json_object_new_int(supported?4:0));
    json_build_object_add(o,"music_supported",json_object_new_boolean(m1_music_supported()));
    mico_rtos_lock_mutex(&m1_lock);bool present=m1_state.mcu_music_present;mico_rtos_unlock_mutex(&m1_lock);
    json_build_object_add(o,"music_present",json_object_new_boolean(present));
    svc_string(o,"tone_min_mcu_version","0.9.3");
    json_build_object_add(o,"tone",json_object_new_int(tone));
    return o;
}
json_object *m1_mcu_json(void) {
    char version[17];bool received,buzzer,updating,buttons,display_source;uint32_t updated,until;unsigned protocol,caps,display_schema;
    mico_rtos_lock_mutex(&m1_lock);
    memcpy(version,m1_state.mcu_version,sizeof(version));received=m1_state.mcu_version_received;
    updated=m1_state.mcu_version_updated_ms;buzzer=m1_state.buzzer_requested;until=m1_state.buzzer_until;
    updating=m1_state.mcu_updating;protocol=m1_state.mcu_protocol;caps=m1_state.mcu_caps;
    buttons=m1_state.mcu_button_events;display_source=m1_state.mcu_display_source;
    display_schema=m1_state.mcu_display_schema;
    mico_rtos_unlock_mutex(&m1_lock);
    uint32_t now=mico_rtos_get_time();
    json_object *o=json_object_new_object();svc_string(o,"version",version);
    json_build_object_add(o,"version_received",json_object_new_boolean(received));
    json_build_object_add(o,"updating",json_object_new_boolean(updating));
    json_build_object_add(o,"extension_protocol",json_object_new_int(protocol));
    json_build_object_add(o,"extension_caps",json_object_new_int(caps));
    json_build_object_add(o,"button_events_supported",json_object_new_boolean(buttons));
    json_build_object_add(o,"display_source_supported",json_object_new_boolean(display_source));
    json_build_object_add(o,"display_source_schema",json_object_new_int(display_schema));
    json_build_object_add(o,"dual_display_source_supported",json_object_new_boolean(display_schema==2));
    if(received)json_build_object_add(o,"version_age_ms",json_object_new_double((uint32_t)(now-updated)));
    json_build_object_add(o,"buzzer_requested",json_object_new_boolean(buzzer && (int32_t)(until-now)>0));
    json_build_object_add(o,"buzzer_remaining_ms",json_object_new_int(buzzer && (int32_t)(until-now)>0?until-now:0));
    svc_string(o,"buzzer_confirmation","command_only");return o;
}
#endif

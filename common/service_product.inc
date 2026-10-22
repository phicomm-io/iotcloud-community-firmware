/* Shared M1/A1 adapter, compiled once per firmware. */
#include "service.h"
#include "status_view.h"
#ifdef FEATURE_DEVICE_A1
#include "a1.h"
#include "filter.h"
#define DEVICE_MODEL "a1"
#define CONFIG a1_config_t
#define STATE a1_state_t
#define GET_CONFIG a1_config_get
#define GET_STATE a1_state_get
#define SAVE_CONFIG a1_config_save
#define STATUS a1_status_json
#define STATUS_LIVE a1_status_live_json
#define REBOOT a1_reboot_later
#define RESET a1_reset
#define WIFI_TRIAL a1_wifi_trial
#define SCAN a1_scan_start
#define DOWNLOAD a1_ota_download
#define DEVICE_NAME_PREFIX "A1"
#define DEVICE_ID a1_id
#else
#include "m1.h"
#define DEVICE_MODEL "m1"
#define CONFIG m1_config_t
#define STATE m1_state_t
#define GET_CONFIG m1_config_get
#define GET_STATE m1_state_get
#define SAVE_CONFIG m1_config_save
#define STATUS m1_status_json
#define STATUS_LIVE m1_status_live_json
#define REBOOT m1_reboot_later
#define RESET m1_reset
#define WIFI_TRIAL m1_wifi_trial
#define SCAN m1_scan_start
#define DOWNLOAD m1_ota_download
#define DEVICE_NAME_PREFIX "M1"
#define DEVICE_ID m1_id
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
#ifndef FEATURE_DEVICE_A1
    json_build_object_add(o,"sensor_session_protocol",json_object_new_int(2));
#endif
    const char *actions[]={"schedules.get","schedules.put","schedules.delete","schedules.enable","buttons.get","status.get","capabilities.get","config.get","logs.get","config.set","wifi.scan","wifi.scan.get","binding.status.get","system.reboot","system.reset","ota.start","night.get","night.set","time.get","time.set","binding.get",
#ifdef FEATURE_DEVICE_A1
    "binding.set",
#endif

#ifdef FEATURE_DEVICE_A1
    "filter.get","filter.set","filter.reset","light_effect.next","light_effect.select","light_always.set","sensors.average.get","buttons.set","binding.probe","light_effect.get","light_effect.set","fan.set","fan.mode.set","fan.mode.toggle","fan.panel","light.set","child_lock.set","linkage.get","linkage.set",
#else
    "display.set",
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
    return o;
}
static __attribute__((noinline)) json_object *service_product_inner(const char *action,json_object *p,const char **code){
    *code="invalid_params";
    if(!strcmp(action,"config.get")){
        if(!svc_keys(p,""))return NULL;CONFIG c;GET_CONFIG(&c);json_object *o=json_object_new_object();
        svc_string(o,"name",c.name);svc_string(o,"ssid",c.ssid);svc_string(o,"mqtt_host",c.mqtt_host);svc_string(o,"mqtt_user",c.mqtt_user);svc_string(o,"mqtt_protocol","tmcp");json_build_object_add(o,"mqtt_protocol_version",json_object_new_int(2));json_build_object_add(o,"mqtt_port",json_object_new_int(c.mqtt_port));
        json_build_object_add(o,"wifi_password_set",json_object_new_boolean(c.wifi_password[0]));json_build_object_add(o,"mqtt_password_set",json_object_new_boolean(c.mqtt_password[0]));json_build_object_add(o,"web_password_set",json_object_new_boolean(c.web_password[0]));
#if defined(FEATURE_DEVICE_A1) && defined(PHICOMM_BEMFA)
        bemfa_config_encode(o);
#endif
        *code="ok";return o;
    }
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
    bool query=strlen(action)>4 && !strcmp(action+strlen(action)-4,".get");
    if(!query && (state.ota_active || state.reboot_pending || state.wifi_trial_active)){*code="busy";return NULL;}
#ifdef FEATURE_DEVICE_A1
    if(!strcmp(action,"filter.reset")){
        if(!svc_keys(p,""))return NULL;
        *code=a1_filter_change(0,true);return !strcmp(*code,"accepted")?a1_filter_json():NULL;
    }
    if(!strcmp(action,"filter.set")){
        int hours;if(!svc_keys(p,"|lifetime_hours|") || !svc_int(p,"lifetime_hours",&hours,1,A1_FILTER_MAX_HOURS))return NULL;
        *code=a1_filter_change(hours,false);return !strcmp(*code,"accepted")?a1_filter_json():NULL;
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
            if(!svc_keys(values,"|name|") || !svc_text(values,"name",c.name,sizeof(c.name),true))return NULL;
            if(!c.name[0]){unsigned suffix=(unsigned)strtoul(DEVICE_ID+8,NULL,16);snprintf(c.name,sizeof(c.name),DEVICE_NAME_PREFIX "-%04X",suffix);}
            ok=SAVE_CONFIG(&c);
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
    unsigned remaining=a1_filter_remaining(&f);
    json_build_object_add(o,"used_seconds",json_object_new_int(f.used_seconds));
    json_build_object_add(o,"lifetime_hours",json_object_new_int(f.lifetime_hours));
    json_build_object_add(o,"remaining_percent",json_object_new_int(remaining));
    json_build_object_add(o,"replacement_due",json_object_new_boolean(remaining<=10));return o;
}
#endif

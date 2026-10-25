#define AUTOMATION_TC1
#include "main.h"
#include "http_server/auth.h"
#include "user_gpio.h"
#include "user_wifi.h"
#include "user_power.h"
#include "mqtt_server/user_mqtt_client.h"
#include "ota_server/signed_adapter.h"
#include "ota_server/ota_server.h"
#include <stdio.h>
#include "../common/i18n.h"
#include "../common/service.c"
#include "../common/status_view.h"
#include "../common/device_status.h"
__attribute__((noinline)) struct printbuf *service_product_status_wire(bool live,bool envelope,const char *id)
{
    device_status_t snapshot;
    device_status_full_t full;
    if(!tc1_status_capture(&snapshot,live?NULL:&full))return NULL;
    return device_status_wire(&snapshot,live?NULL:&full,envelope,id);
}
extern json_object *tc1_status_json(void);
extern json_object *tc1_status_live_json(void);
extern char str_mac[16];
static void text(json_object *o,const char *k,const char *v){json_object_object_add(o,k,json_object_new_string(v));}
static bool keys(json_object *p,const char *allowed){json_object_object_foreach(p,k,v){char needle[80];snprintf(needle,sizeof(needle),"|%s|",k);if(!v || !strstr(allowed,needle))return false;}return true;}
static bool boolparam(json_object *p,const char *k,bool *b){json_object *v=json_object_object_get(p,k);if(!v || !json_object_is_type(v,json_type_boolean))return false;*b=json_object_get_boolean(v);return true;}
static bool number(json_object *p,const char *k,int *n,int lo,int hi){json_object *v=json_object_object_get(p,k);if(!v || !json_object_is_type(v,json_type_int))return false;*n=json_object_get_int(v);return *n>=lo && *n<=hi;}
static bool strparam(json_object *p,const char *k,char *out,size_t cap,bool required){json_object *v=json_object_object_get(p,k);if(!v)return !required;if(!json_object_is_type(v,json_type_string))return false;size_t n=json_object_get_string_len(v);if(n>=cap || n!=strlen(json_object_get_string(v)))return false;memcpy(out,json_object_get_string(v),n+1);return true;}
static void reboot_worker(mico_thread_arg_t arg){(void)arg;mico_rtos_thread_msleep(1500);MicoSystemReboot();mico_rtos_delete_thread(NULL);}
json_object *service_product_capabilities(void){json_object *o=json_object_new_object(),*a=json_object_new_array();text(o,"model","tc1");text(o,"mqtt_protocol",user_config->mqtt_topic_mode?"tmcp":"legacy");json_object_object_add(o,"mqtt_protocol_version",json_object_new_int(user_config->mqtt_topic_mode?2:1));json_object_object_add(o,"protocol",json_object_new_int(2));const char *actions[]={"time.get","time.set","power.get","status.get","capabilities.get","config.get","logs.get","schedules.get","schedules.put","schedules.delete","schedules.enable","buttons.get","buttons.set","config.set","wifi.scan","wifi.scan.get","ota.start","system.reset","system.reboot","outlet.set","indicator.set","child_lock.set",NULL};for(unsigned i=0;actions[i];i++)json_object_array_add(a,json_object_new_string(actions[i]));json_object_object_add(o,"actions",a);status_view_capabilities(o);text(o,"device_language","zh-CN,en");json_build_object_add(o,"config_backup",json_object_new_int(1));json_build_object_add(o,"config_recovery",json_object_new_boolean(device_config_recovery()));json_build_object_add(o,"dual_mqtt",json_object_new_boolean(true));return o;}
static __attribute__((noinline)) json_object *service_product_inner(const char *action,json_object *p,const char **code){
    *code="invalid_params";
    if(!strcmp(action,"config.get")){if(!keys(p,""))return NULL;json_object *o=json_object_new_object();text(o,"migration_state",device_config_recovery()?"confirmation_required":"ready");text(o,"language",ph_language_tag(user_config->bemfa.language));text(o,"name",sys_config->micoSystemConfig.name);text(o,"ssid",sys_config->micoSystemConfig.ssid);text(o,"mqtt_host",user_config->mqtt_host);text(o,"mqtt_user",user_config->mqtt_user);text(o,"mqtt_protocol",user_config->mqtt_topic_mode?"tmcp":"legacy");json_object_object_add(o,"mqtt_protocol_version",json_object_new_int(user_config->mqtt_topic_mode?2:1));json_object_object_add(o,"mqtt_port",json_object_new_int(user_config->mqtt_port));json_object_object_add(o,"mqtt_password_set",json_object_new_boolean(user_config->mqtt_password[0]));json_object_object_add(o,"web_password_set",json_object_new_boolean(user_config->web_password[0]));bemfa_config_encode(o);*code="ok";return o;}
    if(!strcmp(action,"power.get")){if(!keys(p,""))return NULL;json_object *o=json_object_new_object();json_object_object_add(o,"power_watts",json_object_new_double(real_time_power/10.0));json_object_object_add(o,"energy_kwh",json_object_new_double(PowerPulseCountToKwh(p_count)));*code="ok";return o;}
    if(!strcmp(action,"time.get")){if(!keys(p,""))return NULL;json_object *o=json_object_new_object();json_object_object_add(o,"timezone_minutes",json_object_new_int(user_config->automation.timezone));*code="ok";return o;}
    if(!strcmp(action,"wifi.scan.get")){if(!keys(p,""))return NULL;char *raw=WifiScanJson();if(!raw){*code="busy";return NULL;}json_object *out=service_parse(raw,strlen(raw));free(raw);*code="ok";return out;}
    if(!strcmp(action,"ota.start")){char url[512];if(!keys(p,"|url|") || !strparam(p,"url",url,sizeof(url),true))return NULL;*code=ota_server_start(url,NULL,NULL)==kNoErr?"accepted":"busy";return NULL;}
    if(!strcmp(action,"config.set")){
        json_object *section=json_object_object_get(p,"section"),*v=json_object_object_get(p,"values");
        if(keys(p,"|section||values|") && section && json_object_is_type(section,json_type_string) && !strcmp(json_object_get_string(section),"wifi") && v && json_object_is_type(v,json_type_object)){
            char ssid[33],password[65]={0};if(!keys(v,"|ssid||password|") || !strparam(v,"ssid",ssid,sizeof(ssid),true) || !ssid[0])return NULL;
            if(!strcmp(ssid,sys_config->micoSystemConfig.ssid))strcpy(password,sys_config->micoSystemConfig.user_key);
            if(!strparam(v,"password",password,sizeof(password),false))return NULL;*code=WifiConnect(ssid,password)?"accepted":"busy";return NULL;
        }
    }
    if(!tc1_config_reserve()){*code="busy";return NULL;}
    user_config_t *before=malloc(sizeof(*before));if(!before){tc1_config_release();*code="busy";return NULL;}*before=*user_config;
    char old_name[maxNameLen];strcpy(old_name,sys_config->micoSystemConfig.name);
    bool valid=false,enabled=false,saved=true;int index=0;
    if(!strcmp(action,"outlet.set")){
        valid=keys(p,"|index||on|") && boolparam(p,"on",&enabled);
        if(json_object_object_get(p,"index"))valid=valid && number(p,"index",&index,0,5);else index=-1;
        if(valid){if(index<0)UserRelaySetAll(enabled);else UserRelaySet(index,enabled);}
    }else if(!strcmp(action,"indicator.set")){
        valid=keys(p,"|enabled|") && boolparam(p,"enabled",&enabled);if(valid){user_config->power_led_enabled=enabled;UserLedSet(RelayOut() && enabled);}
    }else if(!strcmp(action,"child_lock.set")){
        valid=keys(p,"|enabled|") && boolparam(p,"enabled",&enabled);if(valid){user_config->user[0]=enabled;childLockEnabled=enabled;}
    }else if(!strcmp(action,"system.reboot")){
        valid=keys(p,"");if(valid){saved=mico_rtos_create_thread(NULL,MICO_APPLICATION_PRIORITY,"reboot",reboot_worker,1024,0)==kNoErr;*code=saved?"accepted":"busy";free(before);if(!saved)tc1_config_release();return NULL;}
    }else if(!strcmp(action,"system.reset")){
        valid=keys(p,"|full|") && boolparam(p,"full",&enabled);
        if(valid){
            __typeof__(sys_config->micoSystemConfig) old_system=sys_config->micoSystemConfig;
            bool recovery=device_config_recovery();if(enabled){device_config_recovery_set(false);saved=mico_system_context_restore(sys_config)==kNoErr;}
            else{sys_config->micoSystemConfig.ssid[0]=0;sys_config->micoSystemConfig.user_key[0]=0;sys_config->micoSystemConfig.user_keyLength=0;sys_config->micoSystemConfig.key[0]=0;sys_config->micoSystemConfig.keyLength=0;user_config->web_password[0]=0;}
            if(saved)saved=mico_system_context_update(sys_config)==kNoErr;
            if(!saved){device_config_recovery_set(recovery);*user_config=*before;sys_config->micoSystemConfig=old_system;}
            else saved=mico_rtos_create_thread(NULL,MICO_APPLICATION_PRIORITY,"reset",reboot_worker,1024,0)==kNoErr;
            free(before);if(!saved)tc1_config_release();*code=saved?"accepted":"storage_error";return NULL;
        }
    }else if(!strcmp(action,"wifi.scan")){
        valid=keys(p,"");if(valid){saved=WifiScanStart()==kNoErr;*code=saved?"accepted":"busy";free(before);tc1_config_release();return NULL;}
    }else if(!strcmp(action,"time.set")){
        int zone;valid=keys(p,"|timezone_minutes|") && number(p,"timezone_minutes",&zone,-720,840);if(valid)user_config->automation.timezone=zone;
    }else if(!strcmp(action,"config.set")){
        json_object *section=json_object_object_get(p,"section"),*v=json_object_object_get(p,"values");
        if(keys(p,"|section||values|") && section && json_object_is_type(section,json_type_string) && v && json_object_is_type(v,json_type_object)){
            const char *s=json_object_get_string(section);
            if(!strcmp(s,"device")){char name[maxNameLen],tag[8];strcpy(name,sys_config->micoSystemConfig.name);uint8_t language=user_config->bemfa.language;valid=keys(v,"|name||language|") && (json_object_object_get(v,"name") || json_object_object_get(v,"language")) && strparam(v,"name",name,sizeof(name),false) && (!json_object_object_get(v,"language") || (strparam(v,"language",tag,sizeof(tag),true) && ph_language_parse(tag,&language)));if(valid){user_config->bemfa.language=language;if(name[0])strcpy(sys_config->micoSystemConfig.name,name);else snprintf(sys_config->micoSystemConfig.name,sizeof(sys_config->micoSystemConfig.name),ZTC1_NAME,str_mac+8);}}
            else if(!strcmp(s,"socket")){char name[SOCKET_NAME_LENGTH];valid=keys(v,"|index||name|") && number(v,"index",&index,0,5) && strparam(v,"name",name,sizeof(name),true) && name[0];if(valid)strcpy(user_config->socket_names[index],name);}
            else if(!strcmp(s,"meter")){int n;valid=keys(v,"|coefficient_x100|") && number(v,"coefficient_x100",&n,POWER_COEFFICIENT_MIN_X100,POWER_COEFFICIENT_MAX_X100);if(valid)user_config->power_coefficient_x100=n;}
            else if(!strcmp(s,"report")){int n;valid=keys(v,"|seconds|") && number(v,"seconds",&n,1,3600);if(valid)user_config->mqtt_report_freq=n;}
            else if(!strcmp(s,"auth")){char password[33];valid=keys(v,"|password|") && strparam(v,"password",password,sizeof(password),true);if(valid)strcpy(user_config->web_password,password);}
            else if(!strcmp(s,"bemfa")){bemfa_config_t next;bemfa_config_get(&next);valid=bemfa_config_values(&next,v,true);if(valid){tc1_status_lock();user_config->bemfa=next;tc1_status_unlock();}}
            else if(!strcmp(s,"mqtt")){
                char host[sizeof(user_config->mqtt_host)],user[64],password[257],protocol[8];int port;strcpy(protocol,user_config->mqtt_topic_mode?"tmcp":"legacy");strcpy(password,user_config->mqtt_password);
                bool enabled=user_config->bemfa.mqtt_enabled;
                bool toggle=!json_object_object_get(v,"enabled") || boolparam(v,"enabled",&enabled);
                /* Zero is the factory sentinel for unconfigured MQTT. */
                valid=toggle && keys(v,"|host||port||user||password||protocol||enabled|") && strparam(v,"host",host,sizeof(host),true) && strparam(v,"user",user,sizeof(user),true) && strparam(v,"password",password,sizeof(password),false) && number(v,"port",&port,0,65535) && (port || !host[0]) && strparam(v,"protocol",protocol,sizeof(protocol),false) && (!strcmp(protocol,"tmcp") || !strcmp(protocol,"legacy"));
                if(valid){tc1_status_lock();user_config->bemfa.mqtt_enabled=enabled;user_config->bemfa.revision++;tc1_status_unlock();strcpy(user_config->mqtt_host,host);strcpy(user_config->mqtt_user,user);strcpy(user_config->mqtt_password,password);user_config->mqtt_port=port;user_config->mqtt_topic_mode=!strcmp(protocol,"tmcp");}
            }
        }
    }else{*code="unsupported";free(before);tc1_config_release();return NULL;}
    if(valid)saved=mico_system_context_update(sys_config)==kNoErr;
    /* The SDK caches the HTTP credentials; persistence alone only takes
       effect after reboot. Update the live cache after the Flash commit. */
    if(valid && saved && strcmp(before->web_password,user_config->web_password) &&
       httpd_auth_init(user_config->web_password[0]?"admin":"",user_config->web_password)!=kNoErr){
        *user_config=*before;
        mico_system_context_update(sys_config);
        httpd_auth_init(before->web_password[0]?"admin":"",before->web_password);
        saved=false;
    }
    if(!valid || !saved){tc1_status_lock();*user_config=*before;tc1_status_unlock();strcpy(sys_config->micoSystemConfig.name,old_name);childLockEnabled=user_config->user[0];for(unsigned i=0;i<6;i++)UserRelaySet(i,user_config->socket_status[i]);UserLedSet(RelayOut() && user_config->power_led_enabled);}
    /* HA uses per-entity state topics, not the TMCP status snapshot. Publish
     * committed values after every successful control/config transaction. */
    if(valid && saved){
        bool discovery=before->bemfa.language!=user_config->bemfa.language || strcmp(old_name,sys_config->micoSystemConfig.name)!=0 ||
            memcmp(before->socket_names,user_config->socket_names,sizeof(before->socket_names))!=0;
        UserMqttRefreshState(discovery);
    }
    free(before);tc1_config_release();*code=!valid?"invalid_params":saved?"accepted":"storage_error";return NULL;
}

#include "../common/automation.c"

void automation_load(automation_config_t *out){mico_rtos_suspend_all_thread();*out=user_config->automation;mico_rtos_resume_all_thread();}
bool automation_save(const automation_config_t *next){if(!tc1_config_reserve())return false;automation_config_t *old=malloc(sizeof(*old));if(!old){tc1_config_release();return false;}*old=user_config->automation;bool ok=next->revision==old->revision+1;if(ok){user_config->automation=*next;ok=mico_system_context_update(sys_config)==kNoErr;if(!ok)user_config->automation=*old;}free(old);tc1_config_release();return ok;}

/* Status does not keep configuration/OTA branch scratch on the MQTT stack. */
json_object *service_product(const char *action,json_object *p,const char **code)
{
    if(!strcmp(action,"info.get")){
        bool live;*code="invalid_params";if(!status_view_params(p,&live))return NULL;
        json_object *o=json_object_new_object();text(o,"model","tc1");
        json_build_object_add(o,"device_id",json_object_new_string_len(str_mac,strnlen(str_mac,12)));
        text(o,"version",VERSION);*code=o?"ok":"busy";return o;
    }
    if(!strcmp(action,"status.get")){
        bool live;*code="invalid_params";if(!status_view_params(p,&live))return NULL;
        json_object *o=live?tc1_status_live_json():tc1_status_json();*code=o?"ok":"busy";return o;
    }
    return service_product_inner(action,p,code);
}

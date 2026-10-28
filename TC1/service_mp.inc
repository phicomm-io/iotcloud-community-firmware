/* Native MessagePack TC1 business adapter. Input views are used only during
 * synchronous validation; WifiConnect/OTA copy strings before returning. */
#include "../common/service_mp.h"
static ph_value tc1_mp_get(const ph_value *p,const char *key){ph_value v;phv_get(p,key,&v);return v;}
static bool tc1_mp_has(const ph_value *p,const char *key){ph_value v;return phv_get(p,key,&v);}
static bool keys(const ph_value *p,const char *allowed){return phv_keys(p,allowed);}
static bool boolparam(const ph_value *p,const char *key,bool *value){ph_value v;return phv_get(p,key,&v)&&phv_bool(&v,value);}
static bool number(const ph_value *p,const char *key,int *out,int lo,int hi){ph_value v;int64_t n;if(!phv_get(p,key,&v)||!phv_int(&v,&n)||n<lo||n>hi)return false;*out=(int)n;return true;}
static bool strparam(const ph_value *p,const char *key,char *out,size_t capacity,bool required){ph_value v;if(!phv_get(p,key,&v))return !required;return phv_text(&v,out,capacity);}
static __attribute__((noinline)) bool tc1_product_inner_mp(const char *action,const ph_value *p,ph_writer *out,const char **code){
    *code="invalid_params";
    if(!strcmp(action,"config.get")){
        if(!keys(p,""))return true;
        phw_begin(out,NULL,false);
        phw_text(out,"migration_state",device_config_recovery()?"confirmation_required":"ready");
        phw_text(out,"language",ph_language_tag(user_config->bemfa.language));
        phw_text(out,"name",sys_config->micoSystemConfig.name);phw_text(out,"ssid",sys_config->micoSystemConfig.ssid);
        phw_text(out,"mqtt_host",user_config->mqtt_host);phw_text(out,"mqtt_user",user_config->mqtt_user);
        phw_text(out,"mqtt_protocol",user_config->mqtt_topic_mode?"tmcp":"legacy");
        phw_int(out,"mqtt_protocol_version",user_config->mqtt_topic_mode?2:1);phw_int(out,"mqtt_port",user_config->mqtt_port);
        phw_bool(out,"mqtt_password_set",user_config->mqtt_password[0]);phw_bool(out,"web_password_set",user_config->web_password[0]);
        bemfa_config_mp_fields(out);phw_end(out);*code="ok";return true;
    }
    if(!strcmp(action,"power.get")){
        if(!keys(p,""))return true;
        phw_begin(out,NULL,false);phw_number(out,"power_watts",real_time_power/10.0);
        phw_number(out,"energy_kwh",PowerPulseCountToKwh(p_count));phw_end(out);*code="ok";return true;
    }
    if(!strcmp(action,"time.get")){
        if(!keys(p,""))return true;
        phw_begin(out,NULL,false);phw_int(out,"timezone_minutes",user_config->automation.timezone);phw_end(out);*code="ok";return true;
    }
    if(!strcmp(action,"wifi.scan.get")){
        if(!keys(p,""))return true;
        if(!WifiScanMp(out))*code="busy";else *code="ok";
        return true;
    }
    if(!strcmp(action,"ota.start")){char url[512];if(!keys(p,"|url|") || !strparam(p,"url",url,sizeof(url),true))return true;*code=ota_server_start(url,NULL,NULL)==kNoErr?"accepted":"busy";return true;}
    if(!strcmp(action,"config.set")){
        ph_value section_value=tc1_mp_get(p,"section"),values=tc1_mp_get(p,"values");
        const ph_value *section=&section_value,*v=&values;
        if(keys(p,"|section||values|") && section->kind==PHV_TEXT && phv_equal_text(section,"wifi") && v->kind==PHV_MAP){
            char ssid[33],password[65]={0};if(!keys(v,"|ssid||password|") || !strparam(v,"ssid",ssid,sizeof(ssid),true) || !ssid[0])return true;
            if(!strcmp(ssid,sys_config->micoSystemConfig.ssid))strcpy(password,sys_config->micoSystemConfig.user_key);
            if(!strparam(v,"password",password,sizeof(password),false))return true;*code=WifiConnect(ssid,password)?"accepted":"busy";return true;
        }
    }
    if(!tc1_config_reserve()){*code="busy";return true;}
    user_config_t *before=malloc(sizeof(*before));if(!before){tc1_config_release();*code="busy";return true;}*before=*user_config;
    char old_name[maxNameLen];strcpy(old_name,sys_config->micoSystemConfig.name);
    bool valid=false,enabled=false,saved=true;int index=0;
    if(!strcmp(action,"outlet.set")){
        valid=keys(p,"|index||on|") && boolparam(p,"on",&enabled);
        if(tc1_mp_has(p,"index"))valid=valid && number(p,"index",&index,0,5);else index=-1;
        if(valid){if(index<0)UserRelaySetAll(enabled);else UserRelaySet(index,enabled);}
    }else if(!strcmp(action,"indicator.set")){
        valid=keys(p,"|enabled|") && boolparam(p,"enabled",&enabled);if(valid){user_config->power_led_enabled=enabled;UserLedSet(RelayOut() && enabled);}
    }else if(!strcmp(action,"child_lock.set")){
        valid=keys(p,"|enabled|") && boolparam(p,"enabled",&enabled);if(valid){user_config->user[0]=enabled;childLockEnabled=enabled;}
    }else if(!strcmp(action,"system.reboot")){
        valid=keys(p,"");if(valid){saved=mico_rtos_create_thread(NULL,MICO_APPLICATION_PRIORITY,"reboot",reboot_worker,1024,0)==kNoErr;*code=saved?"accepted":"busy";free(before);if(!saved)tc1_config_release();return true;}
    }else if(!strcmp(action,"system.reset")){
        valid=keys(p,"|full|") && boolparam(p,"full",&enabled);
        if(valid){
            __typeof__(sys_config->micoSystemConfig) old_system=sys_config->micoSystemConfig;
            bool recovery=device_config_recovery();if(enabled){device_config_recovery_set(false);saved=mico_system_context_restore(sys_config)==kNoErr;}
            else{sys_config->micoSystemConfig.ssid[0]=0;sys_config->micoSystemConfig.user_key[0]=0;sys_config->micoSystemConfig.user_keyLength=0;sys_config->micoSystemConfig.key[0]=0;sys_config->micoSystemConfig.keyLength=0;user_config->web_password[0]=0;}
            if(saved)saved=mico_system_context_update(sys_config)==kNoErr;
            if(!saved){device_config_recovery_set(recovery);*user_config=*before;sys_config->micoSystemConfig=old_system;}
            else saved=mico_rtos_create_thread(NULL,MICO_APPLICATION_PRIORITY,"reset",reboot_worker,1024,0)==kNoErr;
            free(before);if(!saved)tc1_config_release();*code=saved?"accepted":"storage_error";return true;
        }
    }else if(!strcmp(action,"wifi.scan")){
        valid=keys(p,"");if(valid){saved=WifiScanStart()==kNoErr;*code=saved?"accepted":"busy";free(before);tc1_config_release();return true;}
    }else if(!strcmp(action,"time.set")){
        int zone;valid=keys(p,"|timezone_minutes|") && number(p,"timezone_minutes",&zone,-720,840);if(valid)user_config->automation.timezone=zone;
    }else if(!strcmp(action,"config.set")){
        ph_value section_value=tc1_mp_get(p,"section"),values=tc1_mp_get(p,"values");
        const ph_value *section=&section_value,*v=&values;
        if(keys(p,"|section||values|") && section->kind==PHV_TEXT && v->kind==PHV_MAP){
            char section_text[32];if(!phv_text(section,section_text,sizeof(section_text)))goto finish;const char *s=section_text;
            if(!strcmp(s,"device")){char name[maxNameLen],tag[8];strcpy(name,sys_config->micoSystemConfig.name);uint8_t language=user_config->bemfa.language;valid=keys(v,"|name||language|") && (tc1_mp_has(v,"name") || tc1_mp_has(v,"language")) && strparam(v,"name",name,sizeof(name),false) && (!tc1_mp_has(v,"language") || (strparam(v,"language",tag,sizeof(tag),true) && ph_language_parse(tag,&language)));if(valid){user_config->bemfa.language=language;if(name[0])strcpy(sys_config->micoSystemConfig.name,name);else snprintf(sys_config->micoSystemConfig.name,sizeof(sys_config->micoSystemConfig.name),ZTC1_NAME,str_mac+8);}}
            else if(!strcmp(s,"socket")){char name[SOCKET_NAME_LENGTH];valid=keys(v,"|index||name|") && number(v,"index",&index,0,5) && strparam(v,"name",name,sizeof(name),true) && name[0];if(valid)strcpy(user_config->socket_names[index],name);}
            else if(!strcmp(s,"meter")){int n;valid=keys(v,"|coefficient_x100|") && number(v,"coefficient_x100",&n,POWER_COEFFICIENT_MIN_X100,POWER_COEFFICIENT_MAX_X100);if(valid)user_config->power_coefficient_x100=n;}
            else if(!strcmp(s,"report")){int n;valid=keys(v,"|seconds|") && number(v,"seconds",&n,1,3600);if(valid)user_config->mqtt_report_freq=n;}
            else if(!strcmp(s,"auth")){char password[33];valid=keys(v,"|password|") && strparam(v,"password",password,sizeof(password),true);if(valid)strcpy(user_config->web_password,password);}
            else if(!strcmp(s,"bemfa")){bemfa_config_t next;bemfa_config_get(&next);valid=bemfa_config_values_mp(&next,v,true);if(valid){tc1_status_lock();user_config->bemfa=next;tc1_status_unlock();}}
            else if(!strcmp(s,"mqtt")){
                char host[sizeof(user_config->mqtt_host)],user[64],password[257],protocol[8];int port;strcpy(protocol,user_config->mqtt_topic_mode?"tmcp":"legacy");strcpy(password,user_config->mqtt_password);
                bool enabled=user_config->bemfa.mqtt_enabled;
                bool toggle=!tc1_mp_has(v,"enabled") || boolparam(v,"enabled",&enabled);
                /* Zero is the factory sentinel for unconfigured MQTT. */
                valid=toggle && keys(v,"|host||port||user||password||protocol||enabled|") && strparam(v,"host",host,sizeof(host),true) && strparam(v,"user",user,sizeof(user),true) && strparam(v,"password",password,sizeof(password),false) && number(v,"port",&port,0,65535) && (port || !host[0]) && strparam(v,"protocol",protocol,sizeof(protocol),false) && (!strcmp(protocol,"tmcp") || !strcmp(protocol,"legacy"));
                if(valid){tc1_status_lock();user_config->bemfa.mqtt_enabled=enabled;user_config->bemfa.revision++;tc1_status_unlock();strcpy(user_config->mqtt_host,host);strcpy(user_config->mqtt_user,user);strcpy(user_config->mqtt_password,password);user_config->mqtt_port=port;user_config->mqtt_topic_mode=!strcmp(protocol,"tmcp");}
            }
        }
    }else{*code="unsupported";free(before);tc1_config_release();return true;}
finish:
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
    free(before);tc1_config_release();*code=!valid?"invalid_params":saved?"accepted":"storage_error";return true;
}

bool service_product_mp(const char *action,const ph_value *params,ph_writer *out,const char **code)
{
    static const char *const actions[]={"config.get","power.get","time.get","wifi.scan.get","ota.start","config.set","outlet.set","indicator.set","child_lock.set","system.reboot","system.reset","wifi.scan","time.set",NULL};
    unsigned i;for(i=0;actions[i];i++)if(!strcmp(actions[i],action))break;
    if(!actions[i])return false;
    unsigned depth=out->depth;size_t length=out->wire->len;
    bool handled=tc1_product_inner_mp(action,params,out,code);
    if(out->depth==depth && out->wire->len==length){phw_begin(out,NULL,false);phw_end(out);}
    return handled;
}

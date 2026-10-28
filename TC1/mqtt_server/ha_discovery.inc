/* HA is a separate JSON surface. Discovery never parses or mutates another
 * JSON document; IDs follow the existing account namespace exactly. */
static void tc1_ha_discovery(unsigned index)
{
    static const char *const entities[]={"socket_0","socket_1","socket_2","socket_3","socket_4","socket_5","total_socket","led","childLock","reboot","soft_reboot","power","powerConsumption","startupTime","powerConsumptionToday","powerConsumptionYesterday","energySession"};
    static const char *const chinese[]={"","","","","","","总开关","指示灯","童锁","重启","软重启","功率","累计用电","运行时间","今日用电","昨日用电","本次启动用电"};
    static const char *const english[]={"","","","","","","All outlets","Indicator","Child lock","Reboot","Soft reboot","Power","Energy Total","Uptime","Energy Today","Energy Yesterday","Energy Since Boot"};
    if(index>=17)return;
    char *bytes=malloc(MAX_MQTT_DATA_SIZE);if(!bytes){if(synchronous_send)synchronous_error=PH_MQTT_MORE;return;}
    char path[MQTT_TOPIC_MAX],text[MQTT_TOPIC_MAX];
    const char *entity=entities[index],*component=index<9?"switch":index<11?"button":"sensor";
    ha_json w;ha_json_init(&w,bytes,MAX_MQTT_DATA_SIZE);ha_json_begin(&w,NULL,false);
    ha_json_string(&w,"name",index<6?user_config->socket_names[index]:ph_i18n(user_config->bemfa.language,chinese[index],english[index]));
    mqtt_identity(&account,entity,text,sizeof(text));ha_json_string(&w,"uniq_id",text);ha_json_string(&w,"object_id",text);
    ha_json_bool(&w,"retain",false);mqtt_leaf(&account,"availability",text,sizeof(text));ha_json_string(&w,"availability_topic",text);
    ha_json_begin(&w,"device",false);ha_json_begin(&w,"identifiers",true);
    mqtt_identity(&account,"",text,sizeof(text));ha_json_string(&w,NULL,text);ha_json_end(&w);
    ha_json_string(&w,"name",sys_config->micoSystemConfig.name);ha_json_string(&w,"model","TC1");ha_json_string(&w,"manufacturer","PHICOMM");ha_json_string(&w,"sw_version",VERSION);ha_json_end(&w);
    if(index<11){
        mqtt_leaf(&account,"ha/command",text,sizeof(text));ha_json_string(&w,"cmd_t",text);
        const char *action=index<7?"outlet.set":index==7?"indicator.set":index==8?"child_lock.set":"system.reboot";
        const char *field=index<7?"on":"enabled";
        if(index>=9){snprintf(text,sizeof(text),"{\"action\":\"%s\",\"params\":{}}",action);ha_json_string(&w,"pl_prs",text);}
        else{
            for(unsigned on=0;on<2;on++){
                if(index<6)snprintf(text,sizeof(text),"{\"action\":\"%s\",\"params\":{\"%s\":%s,\"index\":%u}}",action,field,on?"true":"false",index);
                else snprintf(text,sizeof(text),"{\"action\":\"%s\",\"params\":{\"%s\":%s}}",action,field,on?"true":"false");
                ha_json_string(&w,on?"pl_on":"pl_off",text);
            }
            ha_json_string(&w,"stat_on","ON");ha_json_string(&w,"stat_off","OFF");
            snprintf(text,sizeof(text),"ha/%s/state",entity);mqtt_leaf(&account,text,path,sizeof(path));ha_json_string(&w,"stat_t",path);
        }
        if(index<7)ha_json_string(&w,"device_class","outlet");
    }else{
        mqtt_leaf(&account,"ha/sensor",text,sizeof(text));ha_json_string(&w,"state_topic",text);
        snprintf(text,sizeof(text),"{{ value_json.%s }}",entity);ha_json_string(&w,"value_template",text);
        if(index!=13){ha_json_string(&w,"unit_of_measurement",index==11?"W":"kWh");ha_json_string(&w,"device_class",index==11?"power":"energy");}
        if(index==11)ha_json_string(&w,"state_class","measurement");
        if(index==16)ha_json_string(&w,"state_class","total_increasing");
        if(index==13)ha_json_string(&w,"entity_category","diagnostic");
    }
    mqtt_leaf(&account,"ha/state",text,sizeof(text));ha_json_string(&w,"json_attributes_topic",text);
    ha_json_string(&w,"json_attributes_template","{{ {\"ip\": value_json.ip, \"firmware\": value_json.version} | tojson }}");
    ha_json_end(&w);mqtt_ha(&account,component,entity,path,sizeof(path));
    if(ha_json_done(&w))UserMqttSendTopic(path,bytes,1);else if(synchronous_send)synchronous_error=PH_MQTT_MORE;
    free(bytes);
}
void UserMqttHassAuto(char socket_id){if(socket_id>=1 && socket_id<=6)tc1_ha_discovery((unsigned)socket_id-1);}
void UserMqttHassAutoTotalSocket(void){tc1_ha_discovery(6);}
void UserMqttHassAutoLed(void){tc1_ha_discovery(7);}
void UserMqttHassAutoChildLock(void){tc1_ha_discovery(8);}
void UserMqttHassAutoRebootButton(void){tc1_ha_discovery(9);}
void UserMqttHassAutoSoftRebootButton(void){tc1_ha_discovery(10);}
void UserMqttHassAutoPower(void){for(unsigned i=11;i<17;i++)tc1_ha_discovery(i);}

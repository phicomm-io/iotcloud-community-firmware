static void publish(const String &topic,const String &text,bool retain=true){
  if(!topic||!topic.length()||!text)return;
#ifdef IOTCLOUD_HUB
  if(hubRestartPending())return;
#endif
  if(mqtt&&mqttConnected)esp_mqtt_client_publish(mqtt,topic.c_str(),text.c_str(),text.length(),1,retain);
#ifdef IOTCLOUD_HUB
  hubLocalPublish(topic,text,retain);
#endif
}
static bool mqttStateJson(String &text,void (*fill)(JsonObject),size_t capacity=4096){
  text="";
  for(;;){
    DynamicJsonDocument doc(capacity);
    if(doc.capacity()<capacity)return false;
    fill(doc.to<JsonObject>());
    if(doc.overflowed()){
      if(capacity>=8192)return false;
      capacity*=2;continue;
    }
    const size_t length=measureJson(doc);
    if(!text.reserve(length))return false;
    const size_t written=serializeJson(doc,text);
    return text&&written==length&&text.length()==length;
  }
}
static void httpStatusBody(JsonObject out){
  out["code"]="ok";state(out.createNestedObject("data"));
}
static void publishState(){
  // Release the JSON pool before the SDK/local broker copies the wire payload.
  // Also release the first wire payload before building the typed sensor feed.
  {String text;if(mqttStateJson(text,state))publish(mqttRoot+"/state",text);}
  {String text;if(mqttStateJson(text,sensorBody,2048))publish(sensorRoot+"/state",text,false);}
}

static void discovery(){
  String userKey=config["mqtt"]["user"]|"";String encoded="local";
  if(userKey.length()){encoded="u_";for(size_t i=0;i<userKey.length();i++){char hex[3];snprintf(hex,3,"%02x",(uint8_t)userKey[i]);encoded+=hex;}}
  String haRoot="homeassistant/sensor/"+encoded+"/daikin_"+deviceId+"_";
  const char *classes[]={"pm25","carbon_dioxide","temperature","humidity","volatile_organic_compounds_parts","","pm10"};
  for(unsigned i=0;i<SENSOR_COUNT;i++){
    if(!sensorPresent(i)){publish(haRoot+keys[i]+"/config","");continue;}
    DynamicJsonDocument doc(1536);doc["name"]=ph_i18n(String(config["device"]["language"]|"zh-CN")=="en"?PH_LANG_EN:PH_LANG_ZH,(const char *const[]){"PM2.5","二氧化碳","温度","湿度","总挥发性有机物","甲醛","PM10"}[i],(const char *const[]){"PM2.5","Carbon dioxide","Temperature","Humidity","TVOC","Formaldehyde","PM10"}[i]);doc["unique_id"]=encoded+"_daikin_"+deviceId+"_"+keys[i];
    doc["state_topic"]=mqttRoot+"/state";doc["availability_topic"]=mqttRoot+"/availability";
    doc["value_template"]=String("{{ value_json.sensors.")+keys[i]+".value if value_json.sensors."+keys[i]+".valid else 'None' }}";
    doc["unit_of_measurement"]=units[i];if(*classes[i])doc["device_class"]=classes[i];doc["state_class"]="measurement";doc["expire_after"]=30;
    JsonObject dev=doc.createNestedObject("device");dev["identifiers"]=encoded+"_daikin_"+deviceId;dev["name"]=config["device"]["name"]|"DAIKIN";dev["model"]="BRY88AX151K";dev["manufacturer"]="Daikin";dev["sw_version"]=version;
    String text;serializeJson(doc,text);publish(haRoot+keys[i]+"/config",text);
  }
}
static bool mqttSensorCommand=false;
static volatile bool mqttOnline=false,haBirth=false;
static void mqttEvent(void *,esp_event_base_t,int32_t eventId,void *eventData){
#ifdef IOTCLOUD_HUB
  if(hubRestartPending())return;
#endif
  auto event=(esp_mqtt_event_handle_t)eventData;
  mqttLastEvent=eventId;
  if(eventId==MQTT_EVENT_ERROR&&event->error_handle){mqttErrorType=event->error_handle->error_type;mqttSocketError=event->error_handle->esp_transport_sock_errno;}
  if(eventId==MQTT_EVENT_CONNECTED){mqttConnected=true;mqttCommandReady=false;commandSubId=esp_mqtt_client_subscribe(mqtt,(mqttRoot+"/command").c_str(),1);esp_mqtt_client_subscribe(mqtt,"homeassistant/status",1);esp_mqtt_client_subscribe(mqtt,(sensorRoot+"/command").c_str(),1);}
  if(eventId==MQTT_EVENT_SUBSCRIBED&&event->msg_id==commandSubId){mqttCommandReady=true;mqttOnline=true;}
  if(eventId==MQTT_EVENT_DISCONNECTED){mqttConnected=false;mqttCommandReady=false;}
  #ifdef IOTCLOUD_HUB
  if(eventId==MQTT_EVENT_CONNECTED)hubSubscribe();
  if(eventId==MQTT_EVENT_DATA&&hubCapture(event,0))return;
#endif
  if(eventId!=MQTT_EVENT_DATA||event->retain)return; // Never execute retained commands.
  if(event->current_data_offset==0){
    if(mqttQueued)return;
    if(String(event->topic,event->topic_len)=="homeassistant/status"){if(String(event->data,event->data_len)=="online")haBirth=true;return;}
    mqttSensorCommand=String(event->topic,event->topic_len)==sensorRoot+"/command";
    mqttReceiving=(String(event->topic,event->topic_len)==mqttRoot+"/command"||String(event->topic,event->topic_len)==sensorRoot+"/command")&&event->total_data_len<=4096&&!mqttQueued;
    mqttReceived=0;
  }
  if(!mqttReceiving||event->current_data_offset!=(int)mqttReceived)return;
  memcpy(mqttPending+mqttReceived,event->data,event->data_len);mqttReceived+=event->data_len;
  if(mqttReceived==(size_t)event->total_data_len){mqttPending[mqttReceived]=0;portENTER_CRITICAL(&mqttMux);mqttQueued=true;portEXIT_CRITICAL(&mqttMux);mqttReceiving=false;}
}
static void stopMqtt(){
  if(mqtt){esp_mqtt_client_stop(mqtt);esp_mqtt_client_destroy(mqtt);mqtt=nullptr;}
  mqttConnected=false;mqttCommandReady=false;mqttOnline=false;for(auto &s:sessions)s=SensorSession();
  mqttReceiving=false;mqttReceived=0;mqttSensorCommand=false;
  portENTER_CRITICAL(&mqttMux);mqttQueued=false;mqttPending[0]=0;portEXIT_CRITICAL(&mqttMux);
}
static void startMqtt(){
#ifdef IOTCLOUD_HUB
  if(hubRestartPending())return;
#endif
  stopMqtt();
#ifdef IOTCLOUD_HUB
  hubConfigureUpstream();mqttOnline=hubReady();
#endif
  const char *host=config["mqtt"]["host"]|"";
  String user=config["mqtt"]["user"]|"",userKey="local";
  if(user.length()){userKey="u_";for(size_t i=0;i<user.length();i++){char hex[3];snprintf(hex,3,"%02x",(uint8_t)user[i]);userKey+=hex;}}
  mqttRoot="phicomm/v2/"+userKey+"/daikin/"+deviceId;
  sensorRoot="phicomm/v2/"+userKey+"/sensor/"+deviceId;
  if(!*host)return;
  esp_mqtt_client_config_t c={};
  String clientId="daikin-"+deviceId,availability=mqttRoot+"/availability";
#if ESP_IDF_VERSION_MAJOR >= 5
  c.broker.address.hostname=host;c.broker.address.port=config["mqtt"]["port"]|1883;c.broker.address.transport=MQTT_TRANSPORT_OVER_TCP;
  c.credentials.username=config["mqtt"]["user"]|"";c.credentials.authentication.password=config["mqtt"]["password"]|"";c.credentials.client_id=clientId.c_str();
  c.session.last_will.topic=availability.c_str();c.session.last_will.msg="offline";c.session.last_will.qos=1;c.session.last_will.retain=1;
  c.buffer.size=4608;c.buffer.out_size=4608;c.session.keepalive=30;
#else
  c.host=host;c.port=config["mqtt"]["port"]|1883;c.username=config["mqtt"]["user"]|"";c.password=config["mqtt"]["password"]|"";
  c.client_id=clientId.c_str();c.lwt_topic=availability.c_str();c.lwt_msg="offline";c.lwt_qos=1;c.lwt_retain=1;c.buffer_size=4608;c.out_buffer_size=4608;c.keepalive=30;
#endif
  mqtt=esp_mqtt_client_init(&c);if(mqtt){esp_mqtt_client_register_event(mqtt,MQTT_EVENT_ANY,mqttEvent,nullptr);esp_mqtt_client_start(mqtt);}
}

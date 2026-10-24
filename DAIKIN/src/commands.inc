static void state(JsonObject out){
  uint32_t now=millis();
  out["model"]="daikin";out["hardware_model"]="BRY88AX151K";out["pm_sensor"]=pmModel;
  out["name"]=config["device"]["name"]|"DAIKIN";out["device_id"]=deviceId;out["version"]=version;
  out["uptime_seconds"]=now/1000;out["free_memory"]=ESP.getFreeHeap();
  out["co2_warmup_seconds_remaining"]=now<180000?(180000-now+999)/1000:0;
  out["mqtt_last_event"]=mqttLastEvent;out["mqtt_error_type"]=mqttErrorType;out["mqtt_socket_error"]=mqttSocketError;
  out["button_pressed"]=digitalRead(34)==LOW;
  out["wifi_connected"]=WiFi.status()==WL_CONNECTED;out["ssid"]=config["wifi"]["ssid"]|"";
  out["ip"]=WiFi.localIP().toString();out["rssi"]=WiFi.RSSI();
  out["ap_active"]=bool(WiFi.getMode()&WIFI_AP);out["ap_name"]=apName;
  out["wifi_password_set"]=strlen(config["wifi"]["password"]|"")>0;
  out["web_password_set"]=strlen(config["auth"]["password"]|"")>0;
  out["mqtt_host"]=config["mqtt"]["host"]|"";out["mqtt_port"]=config["mqtt"]["port"]|1883;
  out["mqtt_user"]=config["mqtt"]["user"]|"";out["mqtt_password_set"]=strlen(config["mqtt"]["password"]|"")>0;
  out["mqtt_connected"]=mqttConnected;out["mqtt_status"]=mqttConnected?"connected":strlen(config["mqtt"]["host"]|"")?"connecting":"disabled";
  out["mqtt_protocol"]="tmcp";out["mqtt_topic_mode"]="tmcp";out["mqtt_protocol_version"]=2;
  out["formaldehyde_present"]=hchoPresent;out["indicator_enabled"]=indicator;
  JsonObject sensors=out.createNestedObject("sensors");
  for(int i=0;i<6;i++){
    if(i==5&&!hchoPresent)continue;
    JsonObject s=sensors.createNestedObject(keys[i]);bool valid=readings[i].valid(now);
    s["valid"]=valid;s["received"]=readings[i].received;
    if(valid)s["value"]=readings[i].value;else s["value"]=nullptr;
    if(readings[i].received)s["age_ms"]=uint32_t(now-readings[i].at);else s["age_ms"]=nullptr;
  }
}
static const char *actions[]={"status.get","config.get","capabilities.get","config.set","wifi.scan","wifi.scan.get","indicator.set","system.reboot","system.reset"};
static void capabilities(JsonObject out){
  out["model"]="daikin";out["hardware_model"]="BRY88AX151K";out["protocol"]=2;
  out["mqtt_protocol"]="tmcp";out["mqtt_protocol_version"]=2;
  out["formaldehyde_present"]=hchoPresent;
  JsonArray a=out.createNestedArray("actions");for(auto name:actions)a.add(name);
}
static bool knownFields(JsonObjectConst obj,const char *allowed){
  for(JsonPairConst kv:obj){String needle=String("|")+kv.key().c_str()+"|";if(String(allowed).indexOf(needle)<0)return false;}return true;
}
static bool validText(JsonVariantConst v,size_t max){return v.is<const char*>()&&strlen(v.as<const char*>())<=max;}
static String execute(const String &body){
  DynamicJsonDocument request(6144),reply(8192);
  const char *code="ok",*message="ok";
  if(deserializeJson(request,body)||!request.is<JsonObject>()||!request["action"].is<const char*>()||
     !request["params"].is<JsonObject>()||!knownFields(request.as<JsonObjectConst>(),"|request_id||action||params|")){
    reply["code"]="invalid_params";reply["message"]="Invalid command envelope";String text;serializeJson(reply,text);return text;
  }
  String action=request["action"].as<String>();JsonObjectConst params=request["params"];
  const bool query=action.endsWith(".get");
  String id=request["request_id"]|"";
  if(request.containsKey("request_id")&&(!validText(request["request_id"],64)||id.isEmpty()))code="invalid_params";
  if(!query&&id.isEmpty())code="invalid_params";
  if(!id.isEmpty())reply["request_id"]=id;
  if(!query&&!strcmp(code,"ok"))for(int i=0;i<8;i++)if(lastRequest[i]==id){
    if(lastBody[i]==body)return lastResponse[i];
    reply["code"]="conflict";reply["message"]="request_id reused with different content";String text;serializeJson(reply,text);return text;
  }
  JsonObject data=reply.createNestedObject("data");
  if(!strcmp(code,"ok")){
    if(query&&params.size()!=0)code="invalid_params";
    else if(action=="status.get")state(data);
    else if(action=="capabilities.get")capabilities(data);
    else if(action=="config.get"){
      state(data); // Public settings only; never copy credential-bearing config.
    }else if(action=="wifi.scan"){
      if(params.size())code="invalid_params";
      else if(WiFi.scanComplete()==WIFI_SCAN_RUNNING)code="busy";
      else {WiFi.scanDelete();WiFi.scanNetworks(true);code="accepted";}
    }else if(action=="wifi.scan.get"){
      int n=WiFi.scanComplete();data["scanning"]=n==WIFI_SCAN_RUNNING;
      JsonArray nets=data.createNestedArray("networks");
      for(int i=0;i<n&&i<24;i++){JsonObject net=nets.createNestedObject();net["ssid"]=WiFi.SSID(i);net["rssi"]=WiFi.RSSI(i);net["security"]=WiFi.encryptionType(i)==WIFI_AUTH_OPEN?0:1;}
    }else if(action=="indicator.set"){
      if(!knownFields(params,"|enabled|")||!params["enabled"].is<bool>())code="invalid_params";
      else{DynamicJsonDocument candidate(3072);candidate.set(config);candidate["indicator"]=params["enabled"];if(!saveConfig(candidate))code="storage_error";else{indicator=config["indicator"];data["enabled"]=indicator;}}
    }else if(action=="system.reboot"||action=="system.reset"){
      if(params.size())code="invalid_params";
      else if(action=="system.reset"&&!prefs.clear())code="storage_error";
      else{rebootAt=millis()+1000;code="accepted";}
    }else if(action=="config.set"){
      String section=params["section"]|"";JsonObjectConst values=params["values"];
      bool valid=knownFields(params,"|section||values|")&&values&&!values.isNull()&&values.size()>0;
      if(section=="device")valid=valid&&knownFields(values,"|name|")&&validText(values["name"],63)&&strlen(values["name"]|"")>0;
      else if(section=="auth"){
        valid=valid&&knownFields(values,"|password|")&&validText(values["password"],32);
        size_t n=strlen(values["password"]|"");valid=valid&&(n==0||n>=8);
      }else if(section=="wifi"){
        valid=valid&&knownFields(values,"|ssid||password|")&&validText(values["ssid"],32)&&strlen(values["ssid"]|"")>0;
        if(values.containsKey("password")){size_t n=strlen(values["password"]|"");valid=valid&&validText(values["password"],63)&&(n==0||n>=8);}
      }else if(section=="mqtt"){
        valid=valid&&knownFields(values,"|host||port||user||username||password||protocol|");
        for(JsonPairConst kv:values){String k=kv.key().c_str();if(k=="port")valid=valid&&kv.value().is<int>()&&kv.value().as<int>()>=1&&kv.value().as<int>()<=65535;
          else if(k=="protocol")valid=valid&&String(kv.value().as<const char*>()?kv.value().as<const char*>():"")=="tmcp";
          else valid=valid&&validText(kv.value(),k=="host"?127:k=="password"?256:63);}
      }else valid=false;
      if(!valid)code="invalid_params";
      else{
        DynamicJsonDocument candidate(3072);candidate.set(config);
        // Copy dynamic keys into the candidate's pool: const char* keys would
        // otherwise outlive the request document after config.set(candidate).
        for(JsonPairConst kv:values){const char *key=kv.key().c_str();if(section=="mqtt"&&!strcmp(key,"username"))key="user";candidate[section][String(key)]=kv.value();}
        if(!saveConfig(candidate))code="storage_error";
        else{code="accepted";if(section=="mqtt")mqttRestart=true;if(section=="wifi")wifiRestart=true;}
      }
    }else code="unsupported_action";
  }
  if(reply.overflowed()){reply.clear();code="internal_error";}
  reply["code"]=code;reply["message"]=strcmp(code,"ok")==0||strcmp(code,"accepted")==0?message:code;
  String result;serializeJson(reply,result);
  if(!query&&!id.isEmpty()&&strcmp(code,"invalid_params")!=0){unsigned i=requestSlot++%8;lastRequest[i]=id;lastBody[i]=body;lastResponse[i]=result;}
  return result;
}

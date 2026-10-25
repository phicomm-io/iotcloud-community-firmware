static void otaPartitionStatus(JsonObject out){
  // Report the table installed on this device, independently of the build CSV.
  const auto *running=esp_ota_get_running_partition();
  const auto *next=esp_ota_get_next_update_partition(nullptr);
  out["running"]=running?running->label:nullptr;
  out["running_size"]=running?running->size:0;
  out["next"]=next?next->label:nullptr;
  out["next_size"]=next?next->size:0;
  JsonArray entries=out.createNestedArray("entries");
  auto it=esp_partition_find(ESP_PARTITION_TYPE_ANY,ESP_PARTITION_SUBTYPE_ANY,nullptr);
  while(it){
    const auto *partition=esp_partition_get(it);
    JsonObject entry=entries.createNestedObject();
    entry["label"]=partition->label;entry["type"]=unsigned(partition->type);
    entry["subtype"]=unsigned(partition->subtype);entry["address"]=partition->address;entry["size"]=partition->size;
    it=esp_partition_next(it);
  }
  esp_partition_iterator_release(it);
}
static void state(JsonObject out){
  uint32_t now=millis();
  out["model"]="daikin";out["firmware_variant"]=DAIKIN_VARIANT;out["hardware_model"]="BRY88AX151K";out["pm_sensor"]=pmModel;
  out["migration_state"]=configRecovery?"confirmation_required":"ready";out["language"]=config["device"]["language"]|"zh-CN";out["name"]=config["device"]["name"]|"DAIKIN";out["device_id"]=deviceId;out["version"]=version;
  out["ota_signature_verified"]=prefs.getBool("ota_signed",false);
  otaPartitionStatus(out.createNestedObject("ota_partitions"));
  out["uptime_seconds"]=now/1000;out["free_memory"]=ESP.getFreeHeap();
  out["minimum_free_memory"]=ESP.getMinFreeHeap();out["max_free_block"]=ESP.getMaxAllocHeap();
  out["reset_reason"]=unsigned(esp_reset_reason());
  out["co2_warmup_seconds_remaining"]=!co2Module.warmed&&now<180000?(180000-now+999)/1000:0;
  JsonObject co2=out.createNestedObject("co2_diagnostics");
  co2["protocol"]="cm1106";co2["received"]=co2Module.received;co2["fresh"]=co2Module.fresh(now);
  co2["status_format"]=co2Module.statusFlagsSupported?"bitmask_v06":"legacy_reserved";
  co2["status_flags_supported"]=co2Module.statusFlagsSupported;
  co2["reserved_raw"]=nullptr;if(co2Module.received)co2["reserved_raw"]=co2Module.reserved;
  co2["raw_ppm"]=nullptr;co2["status_raw"]=nullptr;co2["age_ms"]=nullptr;
  if(co2Module.received){co2["raw_ppm"]=co2Module.raw;co2["status_raw"]=co2Module.status;co2["age_ms"]=uint32_t(now-co2Module.measuredAt);}
  const char *flags[]={"preheating","sensor_error","over_range","under_range","uncalibrated","light_aging","drift"};
  for(unsigned i=0;i<7;i++){if(co2Module.received&&co2Module.statusFlagsSupported)co2[flags[i]]=bool(co2Module.status&(1<<i));else co2[flags[i]]=nullptr;}
  co2["frame_errors"]=co2Module.errors;co2["response_timeouts"]=co2Module.timeouts;
  co2["calibration_available"]=co2Module.canCalibrate(now);
  co2["cooldown_seconds"]=co2Module.cooldown(now);
  JsonObject calibration=co2.createNestedObject("calibration");
  calibration["state"]=co2Module.result();calibration["pending"]=co2Module.pending();
  calibration["awaiting_measurement"]=co2Module.awaitingMeasurement;
  if(co2Module.attempted){calibration["reference_ppm"]=co2Module.target;calibration["requested_ms"]=co2Module.requestedAt;}
  if(co2Module.calibration==daikin::CM1106::Acknowledged||co2Module.calibration==daikin::CM1106::Timeout)calibration["completed_ms"]=co2Module.completedAt;
  JsonObject climate=out.createNestedObject("climate_compensation");
  climate["temperature_offset"]=config["climate"]["temperature_offset"]|4.9f;
  climate["humidity_offset"]=config["climate"]["humidity_offset"]|0.0f;
  out["mqtt_last_event"]=mqttLastEvent;out["mqtt_error_type"]=mqttErrorType;out["mqtt_socket_error"]=mqttSocketError;
  out["button_pressed"]=digitalRead(34)==LOW;
  out["wifi_connected"]=WiFi.status()==WL_CONNECTED;out["ssid"]=config["wifi"]["ssid"]|"";
  out["ip"]=(WiFi.status()==WL_CONNECTED?WiFi.localIP():WiFi.softAPIP()).toString();out["rssi"]=WiFi.RSSI();
  out["ap_active"]=bool(WiFi.getMode()&WIFI_AP);out["ap_name"]=apName;
  out["wifi_password_set"]=strlen(config["wifi"]["password"]|"")>0;
  out["web_password_set"]=strlen(config["auth"]["password"]|"")>0;
  out["mqtt_host"]=config["mqtt"]["host"]|"";out["mqtt_port"]=config["mqtt"]["port"]|1883;
  out["mqtt_user"]=config["mqtt"]["user"]|"";out["mqtt_password_set"]=strlen(config["mqtt"]["password"]|"")>0;
  out["mqtt_connected"]=mqttConnected;out["mqtt_status"]=mqttConnected?"connected":strlen(config["mqtt"]["host"]|"")?"connecting":"disabled";
  out["mqtt_protocol"]="tmcp";out["mqtt_topic_mode"]="tmcp";out["mqtt_protocol_version"]=2;
  out["formaldehyde_present"]=hchoPresent;out["indicator_enabled"]=indicator;
  indicatorStatus(out,now);
#ifdef IOTCLOUD_HUB
  JsonObject hub=out.createNestedObject("hub");hub["ready"]=hubReady();hub["clients"]=hubTransport.broker.count();hub["max_clients"]=unsigned(iotcloud::HubBroker::Clients);hub["retained_bytes"]=hubTransport.broker.retainedBytes();hub["refused"]=hubTransport.broker.refused;hub["dropped"]=hubTransport.broker.dropped;hub["account_set"]=hubService.ready();hub["username"]=hubService.user();
  hub["free_byte_heap"]=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  hub["largest_byte_block"]=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  hub["min_byte_heap"]=heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
#endif
  JsonObject sensors=out.createNestedObject("sensors");
  for(unsigned i=0;i<SENSOR_COUNT;i++){
    if(!sensorPresent(i))continue;
    JsonObject s=sensors.createNestedObject(keys[i]);bool valid=readings[i].valid(now);
    s["unit"]=units[i];
    s["valid"]=valid;s["received"]=readings[i].received;
    if(valid)s["value"]=readings[i].value;else s["value"]=nullptr;
    if(readings[i].received)s["age_ms"]=uint32_t(now-readings[i].at);else s["age_ms"]=nullptr;
  }
}
static const char *actions[]={"status.get","config.get","capabilities.get","binding.status.get","config.set","wifi.scan","wifi.scan.get","indicator.set","co2.calibrate","system.reboot","system.reset"};
static void capabilities(JsonObject out){
  out["model"]="daikin";out["firmware_variant"]=DAIKIN_VARIANT;out["hardware_model"]="BRY88AX151K";out["protocol"]=2;
  out["config_backup"]=1;out["config_recovery"]=configRecovery;out["device_language"]="zh-CN,en";out["mqtt_protocol"]="tmcp";out["mqtt_protocol_version"]=2;
  out["formaldehyde_present"]=hchoPresent;out["sensor_session_protocol"]=2;out["sensor_schema"]=1;
  out["indicator_rules_supported"]=true;
#ifndef IOTCLOUD_HUB
  out["ota_variant_conversion"]="full-to-hub";
#endif
  JsonArray a=out.createNestedArray("actions");for(auto name:actions)a.add(name);
}
static bool knownFields(JsonObjectConst obj,const char *allowed){
  for(JsonPairConst kv:obj){String needle=String("|")+kv.key().c_str()+"|";if(String(allowed).indexOf(needle)<0)return false;}return true;
}
static bool validText(JsonVariantConst v,size_t max){return v.is<const char*>()&&strlen(v.as<const char*>())<=max;}
static bool validSection(const String &section,JsonObjectConst values) {
      bool valid=!values.isNull()&&values.size()>0;
      if(section=="device"){
        valid=valid&&knownFields(values,"|name||language|");
        if(values.containsKey("name"))valid=valid&&validText(values["name"],63)&&strlen(values["name"]|"")>0;
        if(values.containsKey("language")){uint8_t language;valid=valid&&validText(values["language"],7)&&ph_language_parse(values["language"].as<const char*>(),&language);}
      }
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
      }else if(section=="climate"){
        valid=valid&&knownFields(values,"|temperature_offset||humidity_offset|")&&values.size()==2&&
          values["temperature_offset"].is<float>()&&values["humidity_offset"].is<float>()&&
          daikin::validClimateOffsets(values["temperature_offset"].as<float>(),values["humidity_offset"].as<float>());
      }else if(section=="indicator_rules"){
        daikin::LightSettings parsed;valid=valid&&daikin::readLightSettings(values,parsed);
      }else valid=false;
      return valid;
}
static bool validConfig(JsonObjectConst candidate){
  #ifdef IOTCLOUD_HUB
  if(candidate.isNull()||!knownFields(candidate,"|_version||device||auth||wifi||mqtt||climate||indicator_rules||indicator||hub|"))return false;
#else
  if(candidate.isNull()||!knownFields(candidate,"|_version||device||auth||wifi||mqtt||climate||indicator_rules||indicator|"))return false;
#endif
  if(candidate.containsKey("_version")&&(!candidate["_version"].is<unsigned>()||candidate["_version"].as<unsigned>()!=1))return false;
  for(JsonPairConst kv:candidate){
    String section=kv.key().c_str();
    if(section=="_version")continue;
#ifdef IOTCLOUD_HUB
    if(section=="hub"){if(!iotcloud::HubService::valid(kv.value()))return false;continue;}
#endif
    if(section=="indicator"){if(!kv.value().is<bool>())return false;}
    else if(!kv.value().is<JsonObjectConst>()||!validSection(section,kv.value().as<JsonObjectConst>()))return false;
  }
  return true;
}
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
#ifdef IOTCLOUD_HUB
  if(!query&&hubRestartPending())code="restart_required";
#endif
  if(!query&&!strcmp(code,"ok"))for(int i=0;i<8;i++)if(lastRequest[i]==id){
    if(lastBody[i]==body)return lastResponse[i];
    reply["code"]="conflict";reply["message"]="request_id reused with different content";String text;serializeJson(reply,text);return text;
  }
  JsonObject data=reply.createNestedObject("data");
  if(!strcmp(code,"ok")){
    if(configRecovery&&!query&&action!="system.reset"&&action!="system.reboot"&&action!="wifi.scan")code="migration_required";
    else if(query&&params.size()!=0)code="invalid_params";
    else if(action=="status.get")state(data);
    else if(action=="binding.status.get")sessionStatus(data);
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
      else{DynamicJsonDocument candidate(configCapacity);candidate.set(config);candidate["indicator"]=params["enabled"];if(!saveConfig(candidate))code="storage_error";else{indicator=config["indicator"];data["enabled"]=indicator;}}
    }else if(action=="co2.calibrate"){
      if(!knownFields(params,"|reference_ppm||reference_stable|")||params.size()!=2||
         !params["reference_ppm"].is<int>()||params["reference_ppm"].as<int>()<400||params["reference_ppm"].as<int>()>1500||
         !params["reference_stable"].is<bool>()||!params["reference_stable"].as<bool>())code="invalid_params";
      else if(!co2Module.request(params["reference_ppm"].as<int>(),millis()))code="not_ready";
      else {readings[CO2].received=false;code="accepted";data["state"]="queued";data["reference_ppm"]=co2Module.target;}
    }else if(action=="system.reboot"||action=="system.reset"){
      if(action=="system.reset" ? (!knownFields(params,"|full|")||(params.containsKey("full")&&!params["full"].is<bool>())) : params.size()!=0)code="invalid_params";
      else if(configRecovery&&action=="system.reset"&&!params["full"].as<bool>())code="migration_required";
#ifdef IOTCLOUD_HUB
      else if(action=="system.reset"&&!homeFactoryReset())code="storage_error";
#endif
      else if(action=="system.reset"&&!prefs.clear())code="storage_error";
      else{
#ifdef IOTCLOUD_HUB
        if(action=="system.reset")hubRestoreChanged();
#endif
        rebootAt=millis()+1000;code="accepted";
      }
    }else if(action=="config.set"){
      String section=params["section"]|"";JsonObjectConst values=params["values"];
      bool valid=knownFields(params,"|section||values|")&&values&&!values.isNull()&&values.size()>0;
      valid=valid&&validSection(section,values);
      if(!valid)code="invalid_params";
      else{
        DynamicJsonDocument candidate(configCapacity);candidate.set(config);
        if(section=="indicator_rules")candidate.remove("indicator_rules");
        // Copy dynamic keys into the candidate's pool: const char* keys would
        // otherwise outlive the request document after config.set(candidate).
        for(JsonPairConst kv:values){const char *key=kv.key().c_str();if(section=="mqtt"&&!strcmp(key,"username"))key="user";candidate[section][String(key)]=kv.value();}
        if(!saveConfig(candidate))code="storage_error";
        else{code="accepted";if(section=="mqtt"||section=="device")mqttRestart=true;if(section=="wifi")wifiRestart=true;
#ifdef IOTCLOUD_HUB
          if(section=="mqtt")hubUpstreamChanged();
#endif
          if(section=="climate"){readings[TEMPERATURE].received=false;readings[HUMIDITY].received=false;nextSample=millis();}
          if(section=="indicator_rules")customLightSettings=daikin::readLightSettings(config["indicator_rules"],lightSettings);}
      }
    }else code="unsupported_action";
  }
  if(reply.overflowed()){reply.clear();code="internal_error";}
  reply["code"]=code;reply["message"]=strcmp(code,"ok")==0||strcmp(code,"accepted")==0?message:code;
  String result;serializeJson(reply,result);
  if(!query&&!id.isEmpty()&&strcmp(code,"invalid_params")!=0){unsigned i=requestSlot++%8;lastRequest[i]=id;lastBody[i]=body;lastResponse[i]=result;}
  return result;
}

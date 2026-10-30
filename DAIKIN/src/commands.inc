#include "diagnostics_ids.h"
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
static void buildBusinessState(JsonObject out,uint32_t now){
  out["snapshot_uptime_ms"]=now;
  out["model"]="daikin";out["firmware_variant"]=DAIKIN_VARIANT;out["hardware_model"]="BRY88AX151K";out["pm_sensor"]=pmModel;
  out["migration_state"]=configRecovery?"confirmation_required":"ready";out["language"]=config["device"]["language"]|"zh-CN";out["name"]=config["device"]["name"]|"DAIKIN";out["device_id"]=deviceId;out["version"]=version;
  out["co2_warmup_seconds_remaining"]=!co2Module.warmed&&now<180000?(180000-now+999)/1000:0;
  JsonObject co2=out.createNestedObject("co2_diagnostics");
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
  JsonObject hub=out.createNestedObject("hub");hub["ready"]=hubReady();hub["account_set"]=hubService.ready();hub["username"]=hubService.user();
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
// The document/source key belong to the application loop. HTTP copies the
// immutable encoded publication through HttpReadSnapshot, never this document.
// Source keys compare exact typed values; no business JSON is built on a hit.
struct BusinessStatusKey {
  struct Sample {float value;uint32_t at;bool received,present,valid;};
  Sample samples[SENSOR_COUNT];
  uint32_t warmup,cooldown,requested,completed;
  unsigned calibration,target,wifiMode;
  int wifiStatus,rssi;
  bool available,pending,awaiting,attempted,button,mqtt,hcho,indicator,recovery,hub;
  String ip;
  bool operator==(const BusinessStatusKey &other)const {
    for(unsigned i=0;i<SENSOR_COUNT;i++){
      const auto &a=samples[i],&b=other.samples[i];
      if(memcmp(&a.value,&b.value,sizeof(float))||a.at!=b.at||a.received!=b.received||a.present!=b.present||a.valid!=b.valid)return false;
    }
    return warmup==other.warmup&&cooldown==other.cooldown&&requested==other.requested&&completed==other.completed&&calibration==other.calibration&&target==other.target&&wifiMode==other.wifiMode&&wifiStatus==other.wifiStatus&&rssi==other.rssi&&available==other.available&&pending==other.pending&&awaiting==other.awaiting&&attempted==other.attempted&&button==other.button&&mqtt==other.mqtt&&hcho==other.hcho&&indicator==other.indicator&&recovery==other.recovery&&hub==other.hub&&ip==other.ip;
  }
};
static bool businessStatusInvalid=true,businessStatusReady=false;
static unsigned businessStatusBuilds=0;
static BusinessStatusKey businessStatusKey;
static DynamicJsonDocument businessStatusDocument(4096);
static HttpReadSnapshot businessReadSnapshot;
static void invalidateBusinessStatusCache(){businessStatusInvalid=true;businessReadSnapshot.invalidate();}
static BusinessStatusKey businessStatusSource(uint32_t now){
  BusinessStatusKey key={};
  for(unsigned i=0;i<SENSOR_COUNT;i++){auto &s=key.samples[i];const auto &r=readings[i];s.value=r.value;s.at=r.at;s.received=r.received;s.present=sensorPresent(i);s.valid=r.valid(now);}
  key.warmup=!co2Module.warmed&&now<180000?(180000-now+999)/1000:0;
  key.cooldown=co2Module.cooldown(now);key.available=co2Module.canCalibrate(now);
  key.requested=co2Module.requestedAt;key.completed=co2Module.completedAt;
  key.calibration=unsigned(co2Module.calibration);key.target=co2Module.target;
  key.pending=co2Module.pending();key.awaiting=co2Module.awaitingMeasurement;key.attempted=co2Module.attempted;
  key.wifiStatus=WiFi.status();key.wifiMode=WiFi.getMode();key.rssi=WiFi.RSSI();
  key.ip=(key.wifiStatus==WL_CONNECTED?WiFi.localIP():WiFi.softAPIP()).toString();
  key.button=digitalRead(34)==LOW;key.mqtt=mqttConnected;key.hcho=hchoPresent;key.indicator=indicator;key.recovery=configRecovery;
#ifdef IOTCLOUD_HUB
  key.hub=hubReady();
#endif
  return key;
}
static bool pollBusinessStatusCache(uint32_t now){
  const BusinessStatusKey next=businessStatusSource(now);
  if(businessStatusReady&&!businessStatusInvalid&&next==businessStatusKey){businessReadSnapshot.refresh(now);return true;}
  // Ordinary updates replace a complete publication only after encoding.
  // A known-invalid sample is different: stop publishing its old valid flag
  // immediately. Configuration changes invalidate explicitly at commit time.
  uint32_t freshFor=0x7fffffffu;
  for(unsigned i=0;i<SENSOR_COUNT;i++){
    if(businessStatusKey.samples[i].valid&&!next.samples[i].valid)businessReadSnapshot.invalidate();
    if(next.samples[i].valid)freshFor=std::min(freshFor,20000u-uint32_t(now-next.samples[i].at));
  }
  businessStatusReady=false;
  businessStatusDocument.clear();buildBusinessState(businessStatusDocument.to<JsonObject>(),now);
  if(businessStatusDocument.overflowed())return false;
  String wire,http;
  const size_t n=measureMsgPack(businessStatusDocument);
  if(!wire.reserve(n)||serializeWireMP(businessStatusDocument,wire)!=n)return false;
  // Reuse the encoded value instead of copying the object tree and encoding
  // the same business data a second time for GET /status.
  ph_writer envelope;if(!phw_init_capacity(&envelope,PH_WIRE_MSGPACK,n+64))return false;
  ph_buf cached={const_cast<char*>(wire.c_str()),wire.length(),wire.length()+1};
  phw_begin(&envelope,nullptr,false);phw_text(&envelope,"code","ok");
  phw_cached_value(&envelope,"data",&cached,8);phw_end(&envelope);
  ph_buf *encoded=phw_take(&envelope);
  const bool complete=encoded&&http.reserve(encoded->len)&&http.concat(encoded->buf,encoded->len);
  ph_buf_free(encoded);if(!complete)return false;
  if(!businessReadSnapshot.publish(std::move(wire),std::move(http),now,freshFor==0x7fffffffu?0:freshFor))return false;
  businessStatusKey=next;businessStatusInvalid=false;businessStatusReady=true;++businessStatusBuilds;return true;
}
static const String &cachedBusinessState(){return businessReadSnapshot.data();}
static const String &cachedHttpStatus(){return businessReadSnapshot.envelope();}
static void state(JsonObject out){
  // Bootstrap is allowed before the first loop; normal reads only copy cache.
  if((!businessStatusReady||businessStatusInvalid)&&!pollBusinessStatusCache(millis()))return;
  out.set(businessStatusDocument.as<JsonObjectConst>());
}
// Only populated diagnostic fields are emitted. A false or zero value is data.
template<class T> static void diagnosticField(JsonArray fields,unsigned id,const T &value){
  JsonArray pair=fields.createNestedArray();pair.add(id);pair.add(value);
}
static void diagnostics(JsonObject out){
  const uint32_t now=millis();out["schema"]=PH_DIAGNOSTICS_SCHEMA;
  JsonArray fields=out.createNestedArray("fields");
  const auto storage=authStorageSnapshot();
  if(storage.attempted)diagnosticField(fields,PH_DIAG_STORAGE_LAST_ERROR,unsigned(storage.error));
  if(storage.sizes){diagnosticField(fields,PH_DIAG_STORAGE_OLD_BLOB_BYTES,storage.oldBytes);diagnosticField(fields,PH_DIAG_STORAGE_NEW_BLOB_BYTES,storage.newBytes);}
  if(storage.required)diagnosticField(fields,PH_DIAG_STORAGE_REQUIRED_ENTRIES,storage.requiredEntries);
  if(storage.stats){diagnosticField(fields,PH_DIAG_STORAGE_NVS_FREE_ENTRIES,storage.freeEntries);diagnosticField(fields,PH_DIAG_STORAGE_NVS_USED_ENTRIES,storage.usedEntries);diagnosticField(fields,PH_DIAG_STORAGE_NVS_TOTAL_ENTRIES,storage.totalEntries);}
  diagnosticField(fields,PH_DIAG_UPTIME_SECONDS,now/1000);
  diagnosticField(fields,PH_DIAG_FREE_MEMORY,ESP.getFreeHeap());
  diagnosticField(fields,PH_DIAG_MINIMUM_FREE_MEMORY,ESP.getMinFreeHeap());
  diagnosticField(fields,PH_DIAG_MAX_FREE_BLOCK,ESP.getMaxAllocHeap());
  diagnosticField(fields,PH_DIAG_RESET_REASON,unsigned(esp_reset_reason()));
  diagnosticField(fields,PH_DIAG_HTTP_SNAPSHOT_UNAVAILABLE,businessReadSnapshot.unavailable());
  diagnosticField(fields,PH_DIAG_HTTP_SNAPSHOT_EXPIRED,businessReadSnapshot.expired());
  diagnosticField(fields,PH_DIAG_HTTP_SNAPSHOT_CONTENDED,businessReadSnapshot.contended());
  diagnosticField(fields,PH_DIAG_HTTP_SNAPSHOT_ALLOCATION_FAILED,businessReadSnapshot.allocationFailed());
  diagnosticField(fields,PH_DIAG_OTA_SIGNATURE_VERIFIED,prefs.getBool("ota_signed",false));
  diagnosticField(fields,PH_DIAG_MQTT_LAST_EVENT,int(mqttLastEvent));
  diagnosticField(fields,PH_DIAG_MQTT_ERROR_TYPE,int(mqttErrorType));
  diagnosticField(fields,PH_DIAG_MQTT_SOCKET_ERROR,int(mqttSocketError));
  diagnosticField(fields,PH_DIAG_CO2_DIAGNOSTICS_PROTOCOL,"cm1106");
  diagnosticField(fields,PH_DIAG_CO2_DIAGNOSTICS_RECEIVED,co2Module.received);
  diagnosticField(fields,PH_DIAG_CO2_DIAGNOSTICS_FRESH,co2Module.fresh(now));
  diagnosticField(fields,PH_DIAG_CO2_DIAGNOSTICS_STATUS_FORMAT,co2Module.statusFlagsSupported?"bitmask_v06":"legacy_reserved");
  diagnosticField(fields,PH_DIAG_CO2_DIAGNOSTICS_STATUS_FLAGS_SUPPORTED,co2Module.statusFlagsSupported);
  diagnosticField(fields,PH_DIAG_CO2_DIAGNOSTICS_FRAME_ERRORS,co2Module.errors);
  diagnosticField(fields,PH_DIAG_CO2_DIAGNOSTICS_RESPONSE_TIMEOUTS,co2Module.timeouts);
  JsonArray partitionPair=fields.createNestedArray();partitionPair.add(PH_DIAG_OTA_PARTITIONS);
  otaPartitionStatus(partitionPair.createNestedObject());
  if(co2Module.received){
    diagnosticField(fields,PH_DIAG_CO2_DIAGNOSTICS_RESERVED_RAW,co2Module.reserved);
    diagnosticField(fields,PH_DIAG_CO2_DIAGNOSTICS_RAW_PPM,co2Module.raw);
    diagnosticField(fields,PH_DIAG_CO2_DIAGNOSTICS_STATUS_RAW,co2Module.status);
    diagnosticField(fields,PH_DIAG_CO2_DIAGNOSTICS_AGE_MS,uint32_t(now-co2Module.measuredAt));
    if(co2Module.statusFlagsSupported){
      const unsigned ids[]={PH_DIAG_CO2_DIAGNOSTICS_PREHEATING,PH_DIAG_CO2_DIAGNOSTICS_SENSOR_ERROR,PH_DIAG_CO2_DIAGNOSTICS_OVER_RANGE,PH_DIAG_CO2_DIAGNOSTICS_UNDER_RANGE,PH_DIAG_CO2_DIAGNOSTICS_UNCALIBRATED,PH_DIAG_CO2_DIAGNOSTICS_LIGHT_AGING,PH_DIAG_CO2_DIAGNOSTICS_DRIFT};
      for(unsigned i=0;i<7;i++)diagnosticField(fields,ids[i],bool(co2Module.status&(1<<i)));
    }
  }
#ifdef IOTCLOUD_HUB
  diagnosticField(fields,PH_DIAG_HUB_FREE_BYTE_HEAP,heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
  diagnosticField(fields,PH_DIAG_HUB_LARGEST_BYTE_BLOCK,heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
  diagnosticField(fields,PH_DIAG_HUB_MIN_BYTE_HEAP,heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
#endif
}
static const char *actions[]={"status.get","diagnostics.get","config.get","capabilities.get","binding.status.get","lan.pair","lan.pair.get","lan.unpair","config.set","wifi.scan","wifi.scan.get","indicator.set","co2.calibrate","system.reboot","system.reset"};
static void capabilities(JsonObject out){
  out["device_id"]=deviceId;out["model"]="daikin";out["firmware_version"]=version;out["variant"]=DAIKIN_VARIANT;out["firmware_variant"]=DAIKIN_VARIANT;out["hardware_model"]="BRY88AX151K";out["protocol"]=2;
  out["config_backup"]=1;out["config_recovery"]=configRecovery;out["device_language"]="zh-CN,en";out["mqtt_protocol"]="tmcp";out["mqtt_protocol_version"]=2;
  out["formaldehyde_present"]=hchoPresent;out["sensor_session_protocol"]=3;out["sensor_schema"]=3;
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
  if(candidate.isNull()||!knownFields(candidate,"|_version||device||auth||wifi||mqtt||climate||indicator_rules||indicator||lan_peers||hub|"))return false;
#else
  if(candidate.isNull()||!knownFields(candidate,"|_version||device||auth||wifi||mqtt||climate||indicator_rules||indicator||lan_peers|"))return false;
#endif
  if(candidate.containsKey("_version")&&(!candidate["_version"].is<unsigned>()||candidate["_version"].as<unsigned>()!=1))return false;
  for(JsonPairConst kv:candidate){
    String section=kv.key().c_str();
    if(section=="_version")continue;
    if(section=="lan_peers"){
      JsonArrayConst peers=kv.value().as<JsonArrayConst>();if(peers.isNull()||peers.size()>12)return false;
      for(JsonObjectConst peer:peers){IPAddress address;if(peer.size()!=2||!knownFields(peer,"|peer||ip|")||!peer["peer"].is<const char*>()||!lc_peer(peer["peer"])||!address.fromString(peer["ip"]|""))return false;}
      continue;
    }
#ifdef IOTCLOUD_HUB
    if(section=="hub"){if(!iotcloud::HubService::valid(kv.value()))return false;continue;}
#endif
    if(section=="indicator"){if(!kv.value().is<bool>())return false;}
    else if(!kv.value().is<JsonObjectConst>()||!validSection(section,kv.value().as<JsonObjectConst>()))return false;
  }
  return true;
}
static String execute(const String &body,bool binary=true){
  DynamicJsonDocument request(6144),reply(8192);
  const char *code="ok",*message="ok";
  if((binary?deserializeMsgPack(request,body.c_str(),body.length()):deserializeJson(request,body))||!request.is<JsonObject>()||!request["action"].is<const char*>()||
     !request["params"].is<JsonObject>()||!knownFields(request.as<JsonObjectConst>(),"|request_id||action||params|")){
    reply["code"]="invalid_params";reply["message"]="Invalid command envelope";String text;if(binary)serializeWireMP(reply,text);else serializeJson(reply,text);return text;
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
    reply["code"]="conflict";reply["message"]="request_id reused with different content";String text;if(binary)serializeWireMP(reply,text);else serializeJson(reply,text);return text;
  }
  JsonObject data=reply.createNestedObject("data");
  if(!strcmp(code,"ok")){
    if(configRecovery&&!query&&action!="system.reset"&&action!="system.reboot"&&action!="wifi.scan")code="migration_required";
    else if(query&&action!="lan.pair.get"&&params.size()!=0)code="invalid_params";
    else if(action=="lan.pair"){
      IPAddress ip;const char *peer=params["peer"]|"",*address=params["ip"]|"",*password=params["password"]|"";
      if(!knownFields(params,"|peer||ip||password||scopes|")||params.size()!=4||!params["password"].is<const char*>()||!params["scopes"].is<unsigned>()||!ip.fromString(address))code="invalid_params";
      else lanPairStart(peer,address,password,params["scopes"].as<unsigned>(),&code);
    }else if(action=="lan.unpair"){if(!knownFields(params,"|peer|")||params.size()!=1)code="invalid_params";else lanUnpairStart(params["peer"]|"",&code);
    }else if(action=="lan.pair.get"){if(!knownFields(params,"|peer|")||params.size()!=1||!lanPairStatus(params["peer"]|"",data))code="invalid_params";}
    else if(action=="status.get")state(data);
    else if(action=="diagnostics.get")diagnostics(data);
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
      else{DynamicJsonDocument candidate(std::min(configCapacity,config.memoryUsage()+1024));candidate.set(config);candidate["indicator"]=params["enabled"];if(!saveConfig(candidate))code="storage_error";else{indicator=config["indicator"];data["enabled"]=indicator;}}
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
      else if(action=="system.reset"&&!resetConfiguration())code="storage_error";
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
        DynamicJsonDocument candidate(std::min(configCapacity,config.memoryUsage()+values.memoryUsage()+1024));candidate.set(config);
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
  String result;if(binary)serializeWireMP(reply,result);else serializeJson(reply,result);
  if(!query&&!id.isEmpty()&&strcmp(code,"invalid_params")!=0){unsigned i=requestSlot++%8;lastRequest[i]=id;lastBody[i]=body;lastResponse[i]=result;}
  return result;
}

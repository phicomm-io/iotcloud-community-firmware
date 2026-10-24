// Account-scoped MQTT authorizes tokens; UDP can only read an existing grant.
static WiFiUDP sensorUdp;
static uint32_t sensorBoot=0,sensorSequence=0;
static String sensorRoot;
struct SensorSession {String id,token;uint32_t boot=0,seen=0;IPAddress ip;bool used=false,active=false;};
static SensorSession sessions[4];
// MiCO represents uint32 counters as signed JSON integers. Decode their bits
// explicitly: ArduinoJson converts a negative integer directly to uint32 as 0.
static bool sessionCounter(JsonVariantConst value,uint32_t &out){
  if(value.is<int32_t>())out=uint32_t(value.as<int32_t>());
  else if(value.is<uint32_t>())out=value.as<uint32_t>();
  else return false;
  return true;
}
static bool hexText(const char *text,size_t length){
  if(!text||strlen(text)!=length)return false;
  for(size_t i=0;i<length;i++)if(!isxdigit((unsigned char)text[i]))return false;
  return true;
}
static void sensorBody(JsonObject out){
  if(!++sensorSequence)++sensorSequence;
  out["schema"]=1;out["device_id"]=deviceId;out["boot"]=int32_t(sensorBoot);out["sequence"]=int32_t(sensorSequence);
  JsonObject values=out.createNestedObject("sensors");
  const uint32_t now=millis();
  for(unsigned i=0;i<SENSOR_COUNT;i++){
    if(!sensorPresent(i))continue;
    JsonObject value=values.createNestedObject(keys[i]);bool valid=readings[i].valid(now);
    value["valid"]=valid;value["unit"]=units[i];
    value["age_ms"]=readings[i].received?uint32_t(now-readings[i].at):0;
    if(valid)value["value"]=readings[i].value;else value["value"]=nullptr;
  }
}
static void sessionStatus(JsonObject out){
  out["max_sessions"]=4;JsonArray peers=out.createNestedArray("peers");
  for(auto &s:sessions)if(s.used&&uint32_t(millis()-s.seen)<1800000){
    JsonObject p=peers.createNestedObject();p["mac"]=s.id;p["ip"]=s.active?s.ip.toString():String("");
    p["session_ready"]=s.active&&uint32_t(millis()-s.seen)<15000;p["idle_ms"]=uint32_t(millis()-s.seen);
  }
}
static bool sessionGrant(const String &text){
  DynamicJsonDocument doc(1024);if(deserializeJson(doc,text))return false;
  if(strcmp(doc["action"]|"","sensor.session.set"))return false;
  JsonObjectConst p=doc["params"];
  uint32_t boot;
  if(!p||p.size()!=4||!p["v"].is<int>()||p["v"].as<int>()!=2||!sessionCounter(p["boot"],boot)||
     !boot||!hexText(p["a1"],12)||!hexText(p["token"],32))return false;
  String id=p["a1"].as<String>(),token=p["token"].as<String>();id.toLowerCase();token.toLowerCase();
  if(id==deviceId||id=="000000000000"||id=="ffffffffffff"||token=="00000000000000000000000000000000")return false;
  SensorSession *slot=nullptr;
  for(auto &s:sessions){if(s.used&&uint32_t(millis()-s.seen)>=1800000)s.used=false;if(s.used&&s.id==id){slot=&s;break;}if(!s.used&&!slot)slot=&s;}
  if(!slot)return false;
  if(slot->id!=id||slot->token!=token||slot->boot!=boot){slot->active=false;slot->ip=IPAddress();}
  slot->id=id;slot->token=token;slot->boot=boot;slot->seen=millis();slot->used=true;return true;
}
static void pollSessions(){
  int size=sensorUdp.parsePacket();if(!size)return;
  if(size>1200){while(sensorUdp.available())sensorUdp.read();return;}
  char wire[1201];int n=sensorUdp.read(wire,size);if(n!=size)return;wire[n]=0;
  DynamicJsonDocument doc(4096);if(deserializeJson(doc,wire))return;
  const char *op=doc["op"]|"";
  uint32_t boot,id;
  if(doc["v"]!=2||deviceId!=(doc["dst"]|"")||(strcmp(op,"read")&&strcmp(op,"probe"))||
     !sessionCounter(doc["id"],id)||!sessionCounter(doc["boot"],boot))return;
  for(auto &s:sessions)if(s.used&&uint32_t(millis()-s.seen)<1800000&&s.id==(doc["src"]|"")&&s.token==(doc["token"]|"")&&s.boot==boot){
    s.ip=sensorUdp.remoteIP();s.seen=millis();s.active=true;
    DynamicJsonDocument reply(4096);reply["v"]=2;reply["src"]=deviceId;reply["dst"]=s.id;reply["boot"]=int32_t(sensorBoot);
    reply["id"]=int32_t(id);reply["token"]=s.token;reply["op"]=!strcmp(op,"read")?"sample":"pong";
    if(!strcmp(op,"read"))sensorBody(reply.createNestedObject("body"));
    String body;serializeJson(reply,body);sensorUdp.beginPacket(s.ip,11288);sensorUdp.write((const uint8_t*)body.c_str(),body.length());sensorUdp.endPacket();break;
  }
}

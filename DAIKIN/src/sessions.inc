// Account-scoped MQTT authorizes tokens; UDP can only read an existing grant.

static uint32_t sensorBoot=0,sensorSequence=0;
static String sensorRoot;
struct SensorSession {String id,token;uint32_t boot=0,seen=0,readAt=0;IPAddress ip;bool used=false,active=false;};
static SensorSession sessions[4];
static uint32_t sensorDescriptorRevision(){uint32_t mask=0;for(unsigned i=0;i<SENSOR_COUNT;i++)if(sensorPresent(i))mask|=1u<<i;return mask+1;}
static void sensorMetadata(JsonObject out){
  out["schema"]=3;out["device_id"]=deviceId;out["boot"]=sensorBoot;out["descriptor_revision"]=sensorDescriptorRevision();
  JsonObject fields=out.createNestedObject("fields");
  const char *names[]={"PM2.5","Carbon dioxide","Temperature","Humidity","TVOC","Formaldehyde","PM10"};
  for(unsigned i=0;i<SENSOR_COUNT;i++)if(sensorPresent(i)){JsonObject field=fields.createNestedObject(keys[i]);field["type"]="number";field["name"]=names[i];field["unit"]=units[i];}
}
static void sensorBody(JsonObject out){
  if(!++sensorSequence)++sensorSequence;
  out["schema"]=3;out["device_id"]=deviceId;out["boot"]=sensorBoot;out["sequence"]=sensorSequence;out["descriptor_revision"]=sensorDescriptorRevision();
  JsonObject values=out.createNestedObject("sensors");const uint32_t now=millis();
  for(unsigned i=0;i<SENSOR_COUNT;i++)if(sensorPresent(i)){
    JsonObject value=values.createNestedObject(keys[i]);bool valid=readings[i].valid(now);value["valid"]=valid;
    value["age_ms"]=readings[i].received?uint32_t(now-readings[i].at):0;
    if(valid)value["value"]=readings[i].value;else value["value"]=nullptr;
  }
}
static void sessionStatus(JsonObject out){
  out["max_sessions"]=4;JsonArray peers=out.createNestedArray("peers");
  for(auto &s:sessions)if(s.used&&uint32_t(millis()-s.seen)<1800000){
    JsonObject p=peers.createNestedObject();p["mac"]=s.id;p["ip"]=s.active?s.ip.toString():String("");
    const uint32_t idle=uint32_t(millis()-(s.active?s.readAt:s.seen));
    p["session_ready"]=s.active&&idle<15000;p["idle_ms"]=idle;
  }
}
static bool sessionGrant(const String &text){
  ph_value root,action,p,value;int64_t version,counter;char peer[13];const void *bytes;size_t length;
  if(!phv_parse(PH_WIRE_MSGPACK,text.c_str(),text.length(),&root)||!phv_get(&root,"action",&action)||!phv_equal_text(&action,"sensor.session.set")||!phv_get(&root,"params",&p)||!phv_keys(&p,"|v||a1||boot||token|"))return false;
  if(!phv_get(&p,"v",&value)||!phv_int(&value,&version)||version!=3||!phv_get(&p,"boot",&value)||!phv_int(&value,&counter)||counter<=0||counter>UINT32_MAX||!phv_get(&p,"a1",&value)||!phv_text(&value,peer,sizeof(peer))||!lc_peer(peer)||!phv_get(&p,"token",&value)||!phv_bytes(&value,&bytes,&length)||length!=16)return false;
  uint8_t bits=0;for(unsigned i=0;i<16;i++)bits|=static_cast<const uint8_t*>(bytes)[i];if(!bits)return false;
  char hex[33];lan_auth_hex(static_cast<const uint8_t*>(bytes),hex);String id=peer,token=hex;uint32_t boot=uint32_t(counter);
  if(id==deviceId||id=="000000000000"||id=="ffffffffffff")return false;
  SensorSession *slot=nullptr;
  for(auto &s:sessions){if(s.used&&uint32_t(millis()-s.seen)>=1800000)s.used=false;if(s.used&&s.id==id){slot=&s;break;}if(!s.used&&!slot)slot=&s;}
  if(!slot)return false;
  if(!slot->used||slot->id!=id||slot->token!=token||slot->boot!=boot){slot->active=false;slot->ip=IPAddress();}
  slot->id=id;slot->token=token;slot->boot=boot;slot->seen=millis();slot->used=true;return true;
}
static void __attribute__((noinline)) pollSessions(){
  int size=sensorUdp.parsePacket();if(!size)return;
  if(size>1200){while(sensorUdp.available())sensorUdp.read();return;}
  char wire[1201];int n=sensorUdp.read(wire,size);if(n!=size)return;wire[n]=0;
  if(configRecovery||!localAuthReady)return;
  ph_value root;if(!phv_parse(PH_WIRE_MSGPACK,wire,size,&root))return;
  String address=sensorUdp.remoteIP().toString();if(lanPairPacket(&root,address.c_str()))return;
  lan_packet request;if(!lan_decode_value(&root,&request)||strcmp(request.dst,deviceId.c_str())||(request.op!=LAN_READ&&request.op!=LAN_PROBE))return;
  bool granted=false;
  for(auto &s:sessions){uint8_t token[16];if(s.used&&uint32_t(millis()-s.seen)<1800000&&s.id==request.src&&s.boot==request.boot&&ph_auth_token_decode(s.token.c_str(),token)&&!memcmp(token,request.token,16)){s.ip=sensorUdp.remoteIP();s.seen=s.readAt=millis();s.active=true;granted=true;break;}}
  if(!granted&&!feature_lan_auth(request.src,request.token,PH_AUTH_SCOPE_SENSOR))return;
  lan_packet reply={};reply.op=request.op==LAN_READ?LAN_SAMPLE:LAN_PONG;strcpy(reply.src,deviceId.c_str());strcpy(reply.dst,request.src);memcpy(reply.token,request.token,16);reply.boot=sensorBoot;reply.id=request.id;
  if(reply.op==LAN_SAMPLE){
    reply.sample.boot=sensorBoot;if(!++sensorSequence)++sensorSequence;reply.sample.sequence=sensorSequence;reply.sample.co2_supported=true;
    const unsigned fields[5]={TEMPERATURE,HUMIDITY,PM25,HCHO,CO2};const double scales[5]={1000,1000,1000,1000000,1};uint32_t now=millis();
    for(unsigned i=0;i<5;i++){const auto &r=readings[fields[i]];reply.sample.age_ms[i]=r.received?uint32_t(now-r.at):0;if(!sensorPresent(fields[i])||!r.valid(now))continue;double value=r.value*scales[i];if(!isfinite(value)||value<INT32_MIN||value>INT32_MAX)continue;int32_t scaled=int32_t(value+(value>=0?.5:-.5));if((i==0&&(scaled < -40000||scaled>125000))||(i==1&&(scaled<0||scaled>100000))||(i>=2&&scaled<0)||(i==4&&(scaled==0||scaled>65535)))continue;reply.sample.value[i]=scaled;reply.sample.fields|=1u<<i;}
  }
  char output[384];ph_buf buffer;ph_writer writer;if(!phw_init_fixed(&writer,PH_WIRE_MSGPACK,&buffer,output,sizeof(output))||!lan_encode(&writer,&reply))return;size_t length=phw_finish_fixed(&writer);if(length)lanSend(address.c_str(),output,length);
}

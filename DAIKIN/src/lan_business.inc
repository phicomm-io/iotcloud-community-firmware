// Application-loop owner only. Borrowed datagrams never survive this call.
#include "../../common/lan_wire_v4.h"
#include <mbedtls/sha256.h>
static lan_v4_server lanBusinessServer={};
static bool lanBusinessContext(const ph_auth_context *context){
  return localAuthReady&&!configRecovery&&context&&context->owner==&localAuth&&
    (context->source==PH_AUTH_UDP_PAIR||context->source==PH_AUTH_UDP_MQTT)&&
    ph_auth_context_valid(context,PH_AUTH_SCOPE_CONTROL);
}
static ph_buf *lanBusinessCopy(const String &text){
  if(!text.length()||text.length()>LAN_V4_RESPONSE_MAX)return nullptr;
  ph_buf *copy=ph_buf_new(text.length()+1);if(!copy)return nullptr;
  memcpy(copy->buf,text.c_str(),text.length());copy->len=text.length();copy->buf[copy->len]=0;return copy;
}
static ph_buf *lanBusinessDispatch(void *,const void *data,size_t length,const ph_auth_context *context,bool *admitted){
  *admitted=false;
  if(otaActive||!lanBusinessContext(context))return nullptr;
  // Admission precedes request allocation/service execution. Do not burn ids
  // when no service could run; after execution the id is always committed.
  if(ESP.getFreeHeap()<24576||ESP.getMaxAllocHeap()<12288)return nullptr;
  ph_value root={},action,id;bool allowed=false;
  if(phv_parse(PH_WIRE_MSGPACK,data,length,&root)&&phv_get(&root,"action",&action))
    allowed=phv_equal_text(&action,"status.get")||phv_equal_text(&action,"capabilities.get")||
      phv_equal_text(&action,"indicator.set")||phv_equal_text(&action,"co2.calibrate")||phv_equal_text(&action,"name.set");
  if(!allowed){
    // CONTROL never inherits ADMIN. Echo a valid request id without borrowing it.
    DynamicJsonDocument reply(512);reply["code"]="unauthorized";
    char requestId[65];if(phv_get(&root,"request_id",&id)&&phv_text(&id,requestId,sizeof(requestId)))reply["request_id"]=String(requestId);
    String wire;serializeWireMP(reply,wire);ph_buf *copy=lanBusinessCopy(wire);
    if(copy)*admitted=true;
    return copy;
  }
  String request(static_cast<const char *>(data),length);
  if(request.length()!=length)return nullptr;
  *admitted=true; // execute may mutate even if its eventual response cannot allocate.
  const String response=execute(request,true);
  return lanBusinessCopy(response);
}
static bool lanBusinessSend(void *,const char *ip,uint16_t port,const void *bytes,size_t length){
  IPAddress address;if(!address.fromString(ip)||!port||!sensorUdp.beginPacket(address,port))return false;
  return sensorUdp.write(static_cast<const uint8_t *>(bytes),length)==length&&sensorUdp.endPacket()==1;
}
static lan_v4_ops lanBusinessOps(){
  lan_v4_ops ops={};ops.local=deviceId.c_str();ops.boot=sensorBoot;
  ops.now=[](void *)->uint32_t{return millis();};
  ops.authorize=[](void *,const char *peer,const uint8_t token[16],ph_auth_context *context){
    char text[33];lan_auth_hex(token,text);
    return localAuthReady&&!configRecovery&&ph_auth_udp_check(&localAuth,peer,text,context)&&lanBusinessContext(context);
  };
  ops.valid=[](void *,const ph_auth_context *context){return lanBusinessContext(context);};
  ops.digest=[](void *,const void *data,size_t length,uint8_t output[32]){return mbedtls_sha256_ret(static_cast<const unsigned char *>(data),length,output,0)==0;};
  ops.dispatch=lanBusinessDispatch;ops.send=lanBusinessSend;return ops;
}
static bool lanBusinessPacket(const ph_value *root,const char *ip,uint16_t port){
  ph_value version;int64_t value;
  if(!phv_get(root,"v",&version)||!phv_int(&version,&value)||value!=4)return false;
  // Consume malformed v4 without allowing it to fall through into v3 handlers.
  lan_v4_packet packet;if(!lan_v4_decode_value(root,&packet))return true;
  if(!localAuthReady||configRecovery)return true;
  const lan_v4_ops ops=lanBusinessOps();char scratch[LAN_V4_FRAME_MAX];
  lan_v4_server_input(&lanBusinessServer,&ops,&packet,ip,port,scratch,sizeof(scratch));return true;
}
static void pollLanBusiness(){
  const lan_v4_ops ops=lanBusinessOps();
  lan_v4_server_expire(&lanBusinessServer,&ops);
  if(otaActive||configRecovery||!localAuthReady)lan_v4_server_release(&lanBusinessServer);
}

static void advertiseLanBusiness(){
  MDNS.addService("openiot","udp",LAN_V4_PORT);
  MDNS.addServiceTxt("openiot","udp","id",deviceId);
  MDNS.addServiceTxt("openiot","udp","model","daikin");
  MDNS.addServiceTxt("openiot","udp","variant",DAIKIN_VARIANT);
  MDNS.addServiceTxt("openiot","udp","firmware",version);
  MDNS.addServiceTxt("openiot","udp","lan_wire","4");
  MDNS.addServiceTxt("openiot","udp","codec","msgpack");
}

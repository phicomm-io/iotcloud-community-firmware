#include <ArduinoJson.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
extern "C" {
#include "protocol/screen_caps.h"
#include "protocol/scene.h"
}
using String=std::string;
static struct {uint32_t session=1,capabilities=SCREEN_CAP_PAGE_CONTROL;} screenBridge;
static struct {bool active=false;} screenImage;
static struct {unsigned mode=0;} screenGuard;
static bool screenUploadBusy=false,screenRecovering=false,guardReady=true,sendOk=true;
static screen_scene screenScene;
static uint32_t ms;
static uint32_t millis(){return ms;}
static bool screenGuardAvailable(){return guardReady;}
static std::vector<uint8_t> sent;
static bool bry_bridge_send(void *,const void *p,size_t n,unsigned cap){
  assert(cap==SCREEN_CAP_PAGE_CONTROL);sent.assign((const uint8_t *)p,(const uint8_t *)p+n);return sendOk;
}
static bool screenFields(JsonObjectConst p,const char *){
  for(auto it:p)if(strcmp(it.key().c_str(),"page")&&strcmp(it.key().c_str(),"direction"))return false;
  return true;
}
#include "screen_page.inc"
int main(){
  scene_defaults(&screenScene);screenScene.pages=3;
  for(unsigned i=0;i<3;i++)screenScene.page[i].enabled=i!=1;
  DynamicJsonDocument params(256),reply(1024);
  auto call=[&](const char *action){reply.clear();return screenPageCommand(action,params.as<JsonObjectConst>(),reply.to<JsonObject>());};
  params.to<JsonObject>();assert(!strcmp(call("screen.page.get"),"ok")&&!reply["available"].as<bool>());
  params["page"]=2;assert(!strcmp(call("screen.page.set"),"invalid_params"));
  params["page"]=-1;assert(!strcmp(call("screen.page.set"),"invalid_params"));
  params.clear();params["direction"]="next";assert(!strcmp(call("screen.page.set"),"accepted"));
  assert(screen_page_request_valid(sent.data(),sent.size())&&sent[2]==2);
  assert(!strcmp(call("screen.page.set"),"busy"));
  uint8_t ack[16]={SCREEN_MSG_PAGE_STATUS,1,0,0,1,0,0,0,3,5,3,0};scene_put32(ack+12,screenScene.revision);
  screenPageReceive(ack,16);assert(!strcmp(screenPage.state,"complete"));
  params.clear();assert(!strcmp(call("screen.page.get"),"ok")&&reply["available"]==true&&reply["current"]==3);
  ack[8]=4;screenPageReceive(ack,16);assert(screenPage.current==3);
  ack[8]=1;ack[3]=1;scene_put32(ack+4,0);screenPageReceive(ack,16);
  assert(screenPage.current==1&&screenPage.asleep&&!strcmp(screenPage.state,"complete"));
  params["page"]=3;screenImage.active=true;assert(!strcmp(call("screen.page.set"),"busy"));screenImage.active=false;
  guardReady=false;assert(!strcmp(call("screen.page.set"),"busy"));guardReady=true;
  sendOk=false;assert(!strcmp(call("screen.page.set"),"busy"));sendOk=true;
  ms=UINT32_MAX-1000;assert(!strcmp(call("screen.page.set"),"accepted"));
  ms=4000;params.clear();assert(!strcmp(call("screen.page.get"),"ok")&&reply["state"]=="timeout");
  screenBridge.session++;assert(!strcmp(call("screen.page.get"),"ok")&&reply["available"]==false);
  ack[3]=0;screenPageReceive(ack,16);assert(screenPage.request==0&&!strcmp(screenPage.state,"idle"));
  screenBridge.capabilities=0;params["page"]=1;assert(!strcmp(call("screen.page.set"),"unsupported"));
}

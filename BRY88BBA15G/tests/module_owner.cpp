#include <ArduinoJson.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
extern "C" {
#include "protocol/screen_caps.h"
#include "protocol/scene.h"
}
static struct { bool extended=true; uint32_t capabilities=SCREEN_CAP_MODULE_CONTROL,session=1; } screenBridge;
static struct { bool active=false; } screenImage;
static struct { unsigned mode=0; } screenGuard;
static bool screenRecovering=false, guardAvailable=true, sendOk=true;
static uint32_t ms;
static std::vector<uint8_t> sent;
static uint32_t millis() { return ms; }
static void delay(unsigned);
static bool screenGuardAvailable() { return guardAvailable; }
static void invalidateBusinessStatusCache() {}
static bool screenFields(JsonObjectConst o,const char *) {
  for (auto p:o) if(strcmp(p.key().c_str(),"set_high") && strcmp(p.key().c_str(),"duration_ms")) return false;
  return true;
}
static bool bry_bridge_send(void *,const void *p,size_t n,unsigned cap) {
  assert(cap==SCREEN_CAP_MODULE_CONTROL);
  sent.assign((const uint8_t*)p,(const uint8_t*)p+n);return sendOk;
}
#include "screen_module.inc"
static bool acknowledge = true;
static void delay(unsigned n) {
  ms += n;
  screenModulePowerPoll(ms);
  if (acknowledge && screenPowerRequest) {
    uint8_t response[20] = {SCREEN_MSG_MODULE_STATUS, 1, 0, 5};
    scene_put32(response + 4, screenPowerRequest);
    scene_put32(response + 8, SCREEN_MODULE_MAX_MS);
    screenModuleReceive(response, sizeof(response));
  }
}
int main() {
  DynamicJsonDocument request(256),reply(1024);
  auto p=request.to<JsonObject>();p["set_high"]=false;p["duration_ms"]=10000;
  auto call=[&](){reply.clear();return screenModuleSet(p,reply.to<JsonObject>());};
  assert(!strcmp(call(),"accepted") && screenModule.request==1);
  assert(sent.size()==12 && sent[2]==SCREEN_MODULE_LOW && scene_u32(sent.data()+8)==10000);
  uint8_t response[20]={SCREEN_MSG_MODULE_STATUS,1,0,5};
  scene_put32(response+4,1);scene_put32(response+8,10000);scene_put32(response+12,40);
  screenModuleReceive(response,20);assert(!strcmp(screenModule.state,"complete"));
  ms=100;reply.clear();screenModuleStatus(reply.to<JsonObject>());
  assert(reply["available"]==true && reply["set_high"]==false && reply["remaining_ms"]==9900);
  response[3]=3;scene_put32(response+4,0);scene_put32(response+8,0);
  screenModuleReceive(response,20);reply.clear();screenModuleStatus(reply.to<JsonObject>());
  assert(reply["set_high"]==true && reply["remaining_ms"]==0 && reply["valid_frames"]==40);
  p["duration_ms"]=60001;assert(!strcmp(call(),"invalid_params"));
  p["duration_ms"]=-1;assert(!strcmp(call(),"invalid_params"));
  p["duration_ms"]=0;assert(!strcmp(call(),"invalid_params"));
  p["duration_ms"]=1;p["set_high"]=true;assert(!strcmp(call(),"invalid_params"));
  p["duration_ms"]=0;screenImage.active=true;assert(!strcmp(call(),"busy"));
  screenImage.active=false;screenRecovering=true;assert(!strcmp(call(),"busy"));
  screenRecovering=false;screenBridge.capabilities=0;assert(!strcmp(call(),"unsupported"));
  screenBridge.capabilities=SCREEN_CAP_MODULE_CONTROL;sendOk=false;assert(!strcmp(call(),"busy"));
  sendOk=true;assert(!strcmp(call(),"accepted"));ms+=3000;
  reply.clear();screenModuleStatus(reply.to<JsonObject>());assert(reply["state"]=="timeout");
  ms+=3000;reply.clear();screenModuleStatus(reply.to<JsonObject>());
  assert(reply["available"]==false && reply["set_high"].isNull());
  screenBridge.session++;reply.clear();screenModuleStatus(reply.to<JsonObject>());
  assert(reply["available"]==false);
  assert(!strcmp(call(),"accepted") && screenModule.request==1);
  p["unknown"]=1;assert(!strcmp(call(),"invalid_params"));
  p.remove("unknown");
  /* HTTP begin waits for the owner and real pin ACK before flash may start. */
  assert(screenModulePowerBegin() && screenModulePowerHealthy());
  assert(sent[2] == SCREEN_MODULE_LOW && scene_u32(sent.data()+8) == 60000);
  assert(!strcmp(call(),"busy"));
  auto first = screenModule.request;
  delay(15000); delay(1);
  assert(screenModule.request == first+1 && screenModulePowerHealthy());
  /* A completed renewal remains valid when flash delays the next owner poll
   * past its ACK deadline and periodic guard announcements are missed. */
  delay(15000);guardAvailable=false;ms+=3500;screenModulePowerPoll(ms);
  assert(screenModulePowerHealthy() && !screenPowerRequest);
  acknowledge=false;delay(15000);delay(3000);
  assert(!screenModulePowerHealthy() && screenPowerFailed);
  guardAvailable=true;
  screenModulePowerEnd(); delay(1);
  assert(!screenModulePowerHealthy() && sent[2] == SCREEN_MODULE_HIGH);
  /* No acknowledgement aborts the prepare within three seconds, then high. */
  acknowledge=false;auto start=ms;
  assert(!screenModulePowerBegin() && ms-start<=4000);
  delay(1);assert(sent[2]==SCREEN_MODULE_HIGH && !screenPowerLeased);
  acknowledge=true;
  /* An expired SET-low status cannot stand in for a renewal ACK. */
  assert(screenModulePowerBegin());delay(15000);
  ms+=SCREEN_MODULE_MAX_MS;screenModulePowerPoll(ms);
  assert(!screenModulePowerHealthy() && screenPowerFailed);
  screenModulePowerEnd();delay(1);
  assert(screenModulePowerBegin());
  screenBridge.session++;delay(1);
  assert(!screenModulePowerHealthy() && screenPowerFailed);
  screenModulePowerEnd();delay(1);
  /* MCU rejection and failed renewals cannot silently keep a flash going. */
  assert(screenModulePowerBegin());
  delay(15000);
  uint8_t rejected[20] = {SCREEN_MSG_MODULE_STATUS,1,2,3};
  scene_put32(rejected+4,screenPowerRequest);
  screenModuleReceive(rejected,20);delay(1);
  assert(!screenModulePowerHealthy());
  screenModulePowerEnd();delay(1);
  /* A live SRAM recovery service already owns SET low. Even when the old
   * business capability cache survives, both uploads must remain usable. */
  screenRecovering=true;screenBridge.capabilities=SCREEN_CAP_MODULE_CONTROL;
  for (unsigned mode:{1u,2u,3u}) {
    screenGuard.mode=mode;auto last=sent;
    assert(screenModulePowerBegin() && screenModulePowerHealthy());
    assert(!screenPowerLeased && sent==last);
    screenModulePowerEnd();delay(1);
  }
  guardAvailable=false;
  assert(!screenModulePowerBegin());screenModulePowerEnd();delay(1);
  guardAvailable=true;screenRecovering=false;screenGuard.mode=0;
  /* A factory peer receives no extension and retains its original OTA path. */
  screenBridge.capabilities=0;auto previous=sent;
  assert(screenModulePowerBegin() && screenModulePowerHealthy() && sent==previous);
  screenModulePowerEnd();delay(1);
}

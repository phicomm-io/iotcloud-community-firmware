#include <ArduinoJson.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include "protocol/guard.h"
static uint32_t ms;
static bool screenStartupAuto=true,save_ok=true;
static unsigned saved,sent;
static struct {
  uint32_t session=1,remaining=30000,seen=0,request=0;
  unsigned mode=1;
  bool activation=true;
  const char *state="ready";
} screenGuard;
static bool screenGuardAvailable(){return screenGuard.session&&uint32_t(ms-screenGuard.seen)<15000;}
static void invalidateBusinessStatusCache(){}
static bool screenSave(){saved++;return save_ok;}
static const char *screenGuardRequest(unsigned op,uint32_t off,unsigned n,JsonObject receipt) {
  assert(op==GUARD_RESUME&&!off&&!n);sent++;screenGuard.request++;
  screenGuard.state="pending";receipt["request"]=screenGuard.request;return "accepted";
}
#include "screen_startup.inc"
int main() {
  auto poll=[](uint32_t at,bool busy=false){ms=at;screenStartupPoll(ms,busy);};
  poll(4999);assert(!sent);poll(5000);assert(sent==1&&screenStartup.attempted);
  screenGuard.state="timeout";poll(6000);assert(sent==1); // No repeated resume after a lost ACK.
  screenGuard.session++;screenGuard.remaining=29000;screenGuard.seen=6000;screenGuard.state="ready";
  screenStartupManual();poll(12000);assert(sent==1); // Manual hold wins even before the first poll.
  screenGuard.session++;screenGuard.seen=12000;screenGuard.remaining=25000;
  poll(12000,true);assert(sent==1);poll(12000);assert(sent==2);
  for(unsigned mode:{0u,2u,3u}) {
    screenGuard.session++;screenGuard.mode=mode;screenGuard.state="ready";
    poll(12000);assert(sent==2);
  }
  screenGuard.mode=1;screenGuard.activation=false;poll(12000);assert(sent==2);
  screenGuard.activation=true;poll(27000);assert(sent==2); // Stale/offline.
  screenGuard.session++;screenGuard.state="ready";screenGuard.seen=0xfffffff0;
  screenGuard.remaining=29900;poll(4884);assert(sent==3); // Millis wrap, MCU elapsed exactly 5s.
  StaticJsonDocument<128> req,out;auto params=req.to<JsonObject>();params["auto_resume"]=false;
  assert(!strcmp(screenStartupSet(params,out.to<JsonObject>()),"ok")&&!screenStartupAuto&&saved==1);
  screenGuard.session++;screenGuard.state="ready";screenGuard.seen=5000;screenGuard.remaining=10000;
  poll(5000);assert(sent==3);
  assert(!strcmp(screenStartupSet(params,out.to<JsonObject>()),"ok")&&saved==1); // Unchanged saves no Flash.
  params["auto_resume"]=true;save_ok=false;
  assert(!strcmp(screenStartupSet(params,out.to<JsonObject>()),"storage_error")&&!screenStartupAuto);
  poll(6000);assert(sent==3);
  save_ok=true;assert(!strcmp(screenStartupSet(params,out.to<JsonObject>()),"ok"));
  screenGuard.state="pending";poll(6000);assert(sent==3);screenGuard.state="complete";poll(6000);assert(sent==4);
  for(const char *bad:{"true","false","1"}){params["auto_resume"]=bad;assert(!strcmp(screenStartupSet(params,out.to<JsonObject>()),"invalid_params"));}
  params["auto_resume"]=1;assert(!strcmp(screenStartupSet(params,out.to<JsonObject>()),"invalid_params"));
  assert(screenStartupAuto);
}

/* Existing GUARD_RESUME protocol; no new MCU command or bootloader write. */
static struct {
  uint32_t session=0,request=0;
  bool manual=false,attempted=false;
} screenStartup;
static void screenStartupSession() {
  if(screenStartup.session!=screenGuard.session) {
    screenStartup={};screenStartup.session=screenGuard.session;
  }
}
static void screenStartupManual() {
  screenStartupSession();screenStartup.manual=true;
}
static void screenStartupGet(JsonObject out) {
  out["auto_resume"]=screenStartupAuto;
  out["delay_seconds"]=5;out["window_seconds"]=30;
  out["manual_hold"]=screenStartup.session==screenGuard.session&&screenStartup.manual;
  out["auto_requested"]=screenStartup.session==screenGuard.session&&screenStartup.attempted;
  out["request"]=screenStartup.session==screenGuard.session?screenStartup.request:0;
  out["session"]=screenGuard.session;
}
static const char *screenStartupSet(JsonObjectConst params,JsonObject out) {
  if(params.size()!=1||!params["auto_resume"].is<bool>())return "invalid_params";
  bool previous=screenStartupAuto;
  screenStartupAuto=params["auto_resume"].as<bool>();
  if(previous!=screenStartupAuto&&!screenSave()) {
    screenStartupAuto=previous;return "storage_error";
  }
  invalidateBusinessStatusCache();screenStartupGet(out);return "ok";
}
static void screenStartupPoll(uint32_t now,bool busy) {
  screenStartupSession();
  if(!screenStartupAuto||screenStartup.manual||screenStartup.attempted||busy||
     !screenGuardAvailable()||!screenGuard.activation||screenGuard.mode!=1||
     !strcmp(screenGuard.state,"pending"))return;
  // Use MCU window time, including ESP restarts and millis() wrap.
  uint32_t age=now-screenGuard.seen;
  uint32_t remaining=screenGuard.remaining>age?screenGuard.remaining-age:0;
  if(remaining>25000)return;
  StaticJsonDocument<128> receipt;
  const char *code=screenGuardRequest(GUARD_RESUME,0,0,receipt.to<JsonObject>());
  if(!strcmp(code,"accepted")) {
    screenStartup.attempted=true;screenStartup.request=screenGuard.request;
    invalidateBusinessStatusCache();
  }
}

/* Read-only MCU PWM telemetry. Never infer an applied value from a setting. */
#include "protocol/backlight.h"
#include "protocol/resources.h"
extern "C" {
#include "protocol/scene.h"
}
static screen_backlight screenBacklight = {};
static uint32_t screenBacklightAt = 0, screenBacklightSession = 0;
static bool screenBacklightReceived = false;
static bool screenBrightnessReceive(const uint8_t *p, size_t n) {
  screen_backlight value;
  if (!(screenBridge.capabilities & SCREEN_CAP_BACKLIGHT) ||
      !screen_backlight_decode(&value, p, n))
    return false;
  screenBacklight = value;
  screenBacklightAt = millis();
  screenBacklightSession = screenBridge.session;
  screenBacklightReceived = true;
  invalidateBusinessStatusCache();
  return true;
}
static void screenBrightnessStatus(JsonObject out) {
  bool fresh = screenBacklightReceived && screenBridge.extended &&
               (screenBridge.capabilities & SCREEN_CAP_BACKLIGHT) &&
               screenBacklightSession == screenBridge.session &&
               !screenRecovering &&
               uint32_t(millis() - screenBacklightAt) < 15000;
  out["available"] = fresh;
  out["supported"] = bool(screenBridge.capabilities & SCREEN_CAP_BACKLIGHT);
  out["requested_percent"] = nullptr;
  out["ceiling_percent"] = nullptr;
  out["applied_percent"] = nullptr;
  out["asleep"] = nullptr;
  out["age_ms"] = nullptr;
  out["target_percent"] = nullptr; out["sleep_reason"] = nullptr;
  out["page"] = nullptr; out["policy_revision"] = nullptr;
  if (fresh) {
    out["requested_percent"] = screenBacklight.requested;
    out["ceiling_percent"] = screenBacklight.ceiling;
    out["applied_percent"] = screenBacklight.applied;
    out["asleep"] = screenBacklight.asleep;
    out["age_ms"] = uint32_t(millis() - screenBacklightAt);
    out["target_percent"] = screenBacklight.target;
    out["sleep_reason"] = screenBacklight.reason;
    out["page"] = screenBacklight.page;
    out["policy_revision"] = screenBacklight.policy_revision;
  }
}

/* Optional font-bank status is read-only and scoped to this MCU session. */
static uint8_t screenResourceState;
static uint16_t screenResourceCount;
static uint32_t screenResourceBytes, screenResourceCrc, screenResourceSession, screenResourceAt;
static bool screenResourceReceived;
static bool screenResourceAdvanced;
static uint8_t screenResourceIo;
static uint32_t screenResourceLoadMs,screenResourceLoaded;
static void screenResourceReceive(const uint8_t *p, size_t n) {
  if (n!=24 || p[1]!=2 || p[0]!=0x2a || p[2]>8 || p[3]>2 ||
      p[6]>3 || p[7] || scene_u32(p+20)>SCREEN_RESOURCE_LIMIT ||
      !(screenBridge.capabilities & SCREEN_CAP_RESOURCE_IO)) return;
  unsigned count=p[4]|p[5]<<8;uint32_t bytes=scene_u32(p+8);
  if(count>8192 || bytes>SCREEN_RESOURCE_LIMIT ||
     (p[2]==2 && (!count || bytes<32+count*24)) ||
     (p[2]!=2 && (count || bytes || scene_u32(p+12))))return;
  screenResourceState=p[2];screenResourceCount=count;screenResourceBytes=bytes;
  screenResourceCrc=scene_u32(p+12);screenResourceSession=screenBridge.session;screenResourceAt=millis();
  screenResourceReceived=true;invalidateBusinessStatusCache();
  screenResourceAdvanced=true;screenResourceIo=p[6];
  screenResourceLoadMs=scene_u32(p+16);screenResourceLoaded=scene_u32(p+20);
}
static void screenResourceStatus(JsonObject out) {
  bool fresh=screenResourceReceived && screenBridge.extended && !screenRecovering &&
       (screenBridge.capabilities & SCREEN_CAP_SCENE_V3) &&
       screenResourceSession==screenBridge.session && uint32_t(millis()-screenResourceAt)<15000;
  static const char *names[]={"unchecked","absent","ready","io_error","invalid","timeout","uploading","preparing","loading"};
  out["available"]=fresh;out["state"]=fresh?names[screenResourceState]:"unknown";
  out["external_font"]=fresh&&screenResourceState==2;
  out["fallback"]=!fresh||screenResourceState!=2;
  out["glyphs"]=fresh?screenResourceCount:0;out["bytes"]=fresh?screenResourceBytes:0;
  out["crc"]=fresh?screenResourceCrc:0;
  if(fresh&&screenResourceAdvanced) {
    static const char *modes[]={"untested","gpio","sfc_single","sfc_quad"};
    out["read_mode"]=modes[screenResourceIo];out["load_ms"]=screenResourceLoadMs;
    out["received"]=screenResourceLoaded;
  }
}

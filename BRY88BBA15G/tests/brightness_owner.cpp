#include <ArduinoJson.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include "protocol/screen_caps.h"
static struct {
  bool extended = true;
  uint32_t session = 123, capabilities = SCREEN_CAP_BACKLIGHT;
} screenBridge;
static bool screenRecovering;
static uint32_t ms, invalidations;
static uint32_t millis() { return ms; }
static void invalidateBusinessStatusCache() { invalidations++; }
#include "screen_brightness.inc"
int main() {
  StaticJsonDocument<512> doc;
  auto status = [&]() {
    doc.clear();
    screenBrightnessStatus(doc.to<JsonObject>());
    assert(!doc.overflowed());
  };
  status();
  assert(!doc["available"].as<bool>() && doc["applied_percent"].isNull());
  uint8_t p[SCREEN_BACKLIGHT_BYTES];
  assert(screen_backlight_encode(p, 60, 10, 10, false));
  ms = UINT32_MAX - 999;
  assert(screenBrightnessReceive(p, sizeof(p)));
  ms = 1000;
  status();
  assert(doc["available"].as<bool>() && doc["age_ms"] == 2000);
  assert(doc["requested_percent"] == 60 && doc["applied_percent"] == 10);
  assert(doc["ceiling_percent"] == 10 && !doc["asleep"].as<bool>());
  p[4] = 11;
  assert(!screenBrightnessReceive(p, sizeof(p)) && invalidations == 1);
  ms = 16000;
  status();
  assert(!doc["available"].as<bool>() && doc["ceiling_percent"].isNull());
  assert(screen_backlight_encode(p, 0, 10, 0, false));
  assert(screenBrightnessReceive(p, sizeof(p)));
  status();
  assert(doc["available"].as<bool>() && doc["applied_percent"] == 0);
  assert(!doc["asleep"].as<bool>());
  assert(screen_backlight_encode(p, 60, 10, 0, true));
  assert(screenBrightnessReceive(p, sizeof(p)));
  status();
  assert(doc["asleep"].as<bool>() && doc["applied_percent"] == 0);
  screenBridge.session++;
  status();
  assert(!doc["available"].as<bool>());
  assert(screenBrightnessReceive(p, sizeof(p)));
  screenRecovering = true;
  status();
  assert(!doc["available"].as<bool>());
  screenRecovering = false;
  screenBridge.extended = false;
  status();
  assert(!doc["available"].as<bool>());
  screenBridge.extended = true;
  screenBridge.capabilities = SCREEN_CAP_SERVICES;
  assert(!screenBrightnessReceive(p, sizeof(p)));
  status();
  assert(!doc["supported"].as<bool>() && !doc["available"].as<bool>());
  screenBridge.capabilities = SCREEN_CAP_BACKLIGHT | SCREEN_CAP_SCENE_V3;
  uint8_t modern[] = {0x27,2,20,40,5,1,5,2,3,7,0,0,0};
  assert(screenBrightnessReceive(modern,sizeof(modern)));
  status();
  assert(doc["target_percent"]==5 && doc["sleep_reason"]==2 &&
         doc["page"]==3 && doc["policy_revision"]==7 && doc["asleep"]==true);
  modern[4]=6;
  assert(!screenBrightnessReceive(modern,sizeof(modern)));
  modern[4]=5;modern[7]=0;
  assert(!screenBrightnessReceive(modern,sizeof(modern)));
  auto resources = [&]() {doc.clear();screenResourceStatus(doc.to<JsonObject>());assert(!doc.overflowed());};
  resources();assert(!doc["available"].as<bool>() && doc["fallback"]==true);
  screenBridge.capabilities|=SCREEN_CAP_RESOURCE_IO;
  uint8_t bank[24] = {0x2a,2,2,0,1,0,0,0,57,0,0,0,0x12,0x34,0x56,0x78};
  ms=0;screenResourceReceive(bank,sizeof(bank));resources();
  assert(doc["available"]==true && doc["external_font"]==true &&
         doc["fallback"]==false && doc["glyphs"]==1 && doc["bytes"]==57);
  assert(doc["crc"].as<uint32_t>()==0x78563412u);
  bank[8]=55;screenResourceReceive(bank,sizeof(bank));resources();
  assert(doc["bytes"]==57);
  ms=15000;resources();assert(doc["available"]==false && doc["fallback"]==true);
  bank[8]=57;screenResourceReceive(bank,sizeof(bank));screenBridge.session++;
  resources();assert(doc["available"]==false);
  bank[2]=4;bank[4]=bank[8]=bank[12]=bank[13]=bank[14]=bank[15]=0;
  screenResourceReceive(bank,sizeof(bank));resources();
  assert(doc["available"]==true && doc["state"]=="invalid" && doc["fallback"]==true);
  screenRecovering=true;resources();assert(doc["available"]==false);screenRecovering=false;
  screenBridge.capabilities&=~SCREEN_CAP_RESOURCE_IO;
  uint8_t extended[24]={0x2a,2,8,0,0,0,3};
  scene_put32(extended+16,1700);scene_put32(extended+20,65536);
  screenResourceReceive(extended,sizeof(extended));resources();assert(doc["state"]=="invalid");
  screenBridge.capabilities|=SCREEN_CAP_RESOURCE_IO;
  screenResourceReceive(extended,sizeof(extended));resources();
  assert(doc["state"]=="loading"&&doc["read_mode"]=="sfc_quad"&&doc["load_ms"]==1700&&doc["received"]==65536);
  extended[6]=4;screenResourceReceive(extended,sizeof(extended));resources();assert(doc["read_mode"]=="sfc_quad");
  extended[6]=1;scene_put32(extended+20,SCREEN_RESOURCE_LIMIT+1);screenResourceReceive(extended,sizeof(extended));resources();assert(doc["read_mode"]=="sfc_quad");
  scene_put32(extended+20,65536);screenResourceReceive(extended,sizeof(extended));resources();assert(doc["read_mode"]=="gpio");
  screenBridge.session++;resources();assert(doc["available"]==false&&doc["read_mode"].isNull());
  screenBridge.capabilities=SCREEN_CAP_BACKLIGHT;resources();assert(doc["available"]==false);
  std::puts("Brightness owner: real MCU status only, stale/session/fault "
            "invalidation, wrap, zero and sleep passed");
}

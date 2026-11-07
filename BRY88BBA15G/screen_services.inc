/* ESP32 application owner. Public commands use the shared openiot dispatcher.
 */
#include "weather.h"
#include <esp_flash.h>
#include <esp_partition.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha256.h>
extern "C" {
#include "bridge.h"
#include "store.h"
#include "image_header.h"
}
static bry_bridge screenBridge = {};
static bry_store screenStore = {};
static SemaphoreHandle_t screenStoreMutex = nullptr;
static bool screenStoreAvailable = false, screenRecovering = false;
static bool screenStartupAuto = true;
static const char *screenStorageError = "not_initialized";
static const char *screenConfigLoadError = "";
static uint32_t screenFlashConfigured = 0, screenFlashPhysical = 0;
#include "protocol/display_policy.h"
#include "protocol/appearance.h"
#include "protocol/wallpaper.h"
#include "protocol/display_diag.h"
#include "protocol/page_control.h"
#include "scene_wire_cache.h"
static screen_appearance screenAppearance;
static uint32_t screenAppearanceAck,screenAppearanceSession,screenAppearanceSentAt;
static unsigned screenAppearanceAttempts;
static const char *screenAppearanceError="";
static void screenWeatherLanguageChanged();
static void screenAppearanceGet(JsonObject,bool wire=false);
static const char *screenAppearanceSet(JsonObjectConst,JsonObject);
static void screenAppearancePoll();
static void screenFontReceive(const uint8_t *,size_t);
static void screenFontPoll(uint32_t);
static const char *screenFontCommand(const String &,JsonObjectConst,JsonObject);
static bool screenFontBusy();
static void screenWallpaperReceive(const uint8_t *,size_t);
static void screenWallpaperPoll(uint32_t);
static void screenWallpaperGet(JsonObject);
static const char *screenWallpaperCommand(const String &,JsonObjectConst,JsonObject);
static display_policy screenDisplayPolicy;
static uint32_t screenPolicySession, screenPolicyAck, screenPolicySentAt;
static const char *screenPolicyError = "";
static void screenPolicyGet(JsonObject);
static const char *screenPolicySet(JsonObjectConst, JsonObject);
static void screenPolicyPoll();
static screen_scene screenScene;
static uint32_t screenSyncSize = SCENE_BYTES;
#include "layout_upload.h"
static ScreenLayoutUpload screenLayoutUpload;
static reminder_schedule screenSchedule = {};
static reminder_notice screenNotices[REMINDER_MAX] = {};
static screen_weather screenWeather = {};
static constexpr size_t screenWeatherConfigBytes = 1536;
static DynamicJsonDocument screenWeatherConfig(screenWeatherConfigBytes), screenSavedSettings(1024);
static uint32_t screenSettingsSession = 0;
static uint32_t screenSceneAck = 0, screenSceneSession = 0,
                screenSyncOffset = 0, screenSyncDeadline = 0,
                screenSyncRetryAt = 0;
static unsigned screenSyncStep = 0;
static uint32_t screenServicesAt = 0, screenNoticeId = 0x80000000u;
static SceneWireCache<> screenSceneWire;
static const char *screenSyncError = "";
static void screenModuleReceive(const uint8_t *, size_t);
static void screenDiagReceive(const uint8_t *, size_t);
static void screenPageReceive(const uint8_t *, size_t);
static void screenPageStatus(JsonObject);
static bool screenFields(JsonObjectConst, const char *);
struct ScreenImage {
  bool valid = false, active = false, pending = false, bootstrap = false;
  uint32_t features = 0;
  uint32_t size = 0, offset = 0, chunk = 0, deadline = 0;
  uint8_t md5[16] = {}, sha[32] = {};
  const char *state = "empty";
};
static ScreenImage screenImage;
#include "screen_brightness.inc"
#include "screen_config_document.h"
#include "screen_store_writer.h"
#include "screen_flash_erase.h"
static void screenGetSettings(JsonObject data) {
  data["settings"] = screenSnapshot["setting"];
  data["saved_settings"] = screenSavedSettings;
  data["snapshot_age_ms"] = nullptr;
  if (screenSnapshot.containsKey("setting"))
    data["snapshot_age_ms"] = uint32_t(millis() - screenSnapshotAt);
  data["request"]["token"] = screenPendingToken;
  data["request"]["code"] = screenPendingCode;
  data["request"]["state"] = screenSettingsRequestState();
  screenBrightnessStatus(data.createNestedObject("brightness"));
}
static esp_partition_t screenStorePartition = {};
static bool screenReadStore(void *, uint32_t a, void *b, size_t n) {
  bool ok = esp_partition_read(&screenStorePartition, a, b, n) == ESP_OK;
  /* Blank-tail probing and full image verification also yield to idle/Wi-Fi. */
  if (!(a & 0x3fff))
    delay(1);
  return ok;
}
static bool screenWriteStore(void *, uint32_t a, const void *b, size_t n) {
  return esp_partition_write(&screenStorePartition, a, b, n) == ESP_OK;
}
static bool screenEraseStore(void *, uint32_t a, size_t n) {
  return screenEraseSectors(a, n, screenStorePartition.size,
      [](uint32_t address, size_t size) {
        return esp_partition_erase_range(&screenStorePartition, address, size) == ESP_OK;
      }, [] { delay(10); });
}
static bool screenOpenStorage() {
  screenStorageError = "flash_geometry";
  if (esp_flash_get_size(nullptr, &screenFlashConfigured) != ESP_OK ||
      esp_flash_get_physical_size(nullptr, &screenFlashPhysical) != ESP_OK ||
      !esp_flash_default_chip || screenFlashPhysical != 0x800000 ||
      (screenFlashConfigured != 0x400000 && screenFlashConfigured != 0x800000))
    return false;
  screenStorageError = "partition_layout";
  struct Part {
    const char *name;
    uint32_t address, size;
    uint8_t type, subtype;
  };
  const Part expected[] = {{"nvs", 0x9000, 0x4000, 1, 2},
                           {"otadata", 0xd000, 0x2000, 1, 0},
                           {"phy_init", 0xf000, 0x1000, 1, 1},
                           {"ota_0", 0x10000, 0x1e0000, 0, 0x10},
                           {"ota_1", 0x1f0000, 0x1e0000, 0, 0x11}};
  unsigned found = 0;
  auto it = esp_partition_find(ESP_PARTITION_TYPE_ANY,
                               ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (it) {
    const esp_partition_t *p = esp_partition_get(it);
    bool match = false;
    if (p && !p->encrypted && p->flash_chip == esp_flash_default_chip)
      for (unsigned i = 0; i < 5; i++)
        if (!strcmp(p->label, expected[i].name) &&
            p->address == expected[i].address && p->size == expected[i].size &&
            p->type == expected[i].type && p->subtype == expected[i].subtype &&
            !(found & (1u << i))) {
          found |= 1u << i;
          match = true;
          break;
        }
    if (!match) {
      esp_partition_iterator_release(it);
      return false;
    }
    it = esp_partition_next(it);
  }
  if (found != 31)
    return false;
  /* The preserved factory bootloader declares 4 MiB on an 8 MiB board. IDF
   * clips this RAM field to that header, including bounds for explicit reads.
   * Restore the physically detected size only after checking the exact table.
   * No bootloader, table, flash mode, ROM/cache mapping or eFuse is changed.
   * IDF 4.4: esp_flash_init_default_chip / esp_flash_get_physical_size.
   */
  esp_flash_default_chip->size = screenFlashPhysical;
  screenStorePartition.flash_chip = esp_flash_default_chip;
  screenStorePartition.address = 0x700000;
  screenStorePartition.size = BRY_STORE_SIZE;
  screenStorePartition.type = ESP_PARTITION_TYPE_DATA;
  screenStorePartition.subtype = (esp_partition_subtype_t)0x40;
  strcpy(screenStorePartition.label, "iotc_screen");
  screenStore.read = screenReadStore;
  screenStore.write = screenWriteStore;
  screenStore.erase = screenEraseStore;
  if (!bry_store_probe(&screenStore)) {
    esp_flash_default_chip->size = screenFlashConfigured;
    screenStorageError = "tail_occupied_or_unreadable";
    return false;
  }
  screenStorageError = "";
  return true;
}
static String screenBase64(const uint8_t *p, size_t n) {
  // Keep one full output buffer. A second SCN3-sized temporary splits the
  // available heap and can prevent allocation of the durable config record.
  String result;
  const size_t total = (n + 2) / 3 * 4;
  if (!result.reserve(total)) return String();
  uint8_t block[257];
  for (size_t at = 0; at < n; at += 192) {
    size_t count = n - at, used = 0;
    if (count > 192) count = 192; // Full chunks must end on a 3-byte boundary.
    if (mbedtls_base64_encode(block, sizeof(block), &used, p + at, count) ||
        !result.concat((const char *)block, used))
      return String();
  }
  if (result.length() != total) return String();
  // Return the named local so String moves its buffer. A conditional return
  // expression would copy this lvalue into another full-size allocation.
  return result;
}
static bool screenUnbase64(JsonVariantConst v, uint8_t *p, size_t cap,
                           size_t &used) {
  if (!v.is<const char *>())
    return false;
  const char *s = v.as<const char *>();
  size_t n = strlen(s);
  if (n > 4 * ((cap + 2) / 3))
    return false;
  used = 0;
  return mbedtls_base64_decode(p, cap, &used, (const uint8_t *)s, n) == 0;
}
static String screenSceneBase64() {
  String result;
  const size_t total = (SCENE_BYTES + 2) / 3 * 4;
  if (!result.reserve(total)) return String();
  uint8_t input[192], output[257];
  for (size_t at = 0; at < SCENE_BYTES; at += sizeof(input)) {
    size_t count = min(sizeof(input), size_t(SCENE_BYTES - at)), used = 0;
    if (!screenSceneWire.read(at, input, count) ||
        mbedtls_base64_encode(output, sizeof(output), &used, input, count) ||
        !result.concat((const char *)output, used)) return String();
  }
  return result;
}
static bool screenSave() {
  if (!screenStoreAvailable || !screenStoreMutex)
    return false;
  if (xSemaphoreTake(screenStoreMutex, 0) != pdTRUE) {
    screenStorageError = "config_busy";
    return false;
  }
  screenStorageError = "config_layout_encode";
  if (!screenSceneWire.prepare(screenScene)) {
    xSemaphoreGive(screenStoreMutex);
    return false;
  }
  String layout = screenSceneBase64();
  // A String allocation failure must not commit an empty layout under a
  // valid record CRC. Keep the prior slot intact under TLS heap pressure.
  screenStorageError = "config_layout_allocation";
  if (layout.length() != 4 * ((scene_wire_size(&screenScene) + 2) / 3)) {
    xSemaphoreGive(screenStoreMutex);
    return false;
  }
  const size_t capacity = screenConfigCapacity(screenWeatherConfig,
      screenSavedSettings, screenSchedule, screenNotices);
  uint8_t policyBytes[DISPLAY_POLICY_BYTES];
  screenStorageError = "config_policy_encode";
  if (!display_policy_encode(&screenDisplayPolicy, policyBytes, sizeof(policyBytes))) {
    xSemaphoreGive(screenStoreMutex); return false;
  }
  String policy = screenBase64(policyBytes, sizeof(policyBytes));
  screenStorageError = "config_policy_allocation";
  if (policy.length() != 4 * ((DISPLAY_POLICY_BYTES + 2) / 3)) {
    xSemaphoreGive(screenStoreMutex); return false;
  }
  uint8_t appearanceBytes[APPEARANCE_BYTES];
  if(!appearance_encode(&screenAppearance,appearanceBytes,sizeof(appearanceBytes))){xSemaphoreGive(screenStoreMutex);return false;}
  String appearance=screenBase64(appearanceBytes,sizeof(appearanceBytes));
  if(appearance.length()!=4*((APPEARANCE_BYTES+2)/3)){xSemaphoreGive(screenStoreMutex);return false;}
  DynamicJsonDocument doc(capacity);
  screenStorageError = "config_document_allocation";
  if (doc.capacity() < capacity) {
    xSemaphoreGive(screenStoreMutex);
    return false;
  }
  screenStorageError = "config_document_overflow";
  if (!screenConfigDocument(doc, layout.c_str(), screenWeatherConfig,
                            screenSavedSettings, screenSchedule, screenNotices, policy.c_str(), appearance.c_str(), screenStartupAuto)) {
    xSemaphoreGive(screenStoreMutex);
    return false;
  }
  /* Stream to the inactive slot, then commit its header and verify readback.
   * No second complete record allocation competes with SCN3 or TLS buffers. */
  const size_t size = measureMsgPack(doc);
  screenStorageError = "config_record_size";
  bool ok = false;
  if (!doc.overflowed() && size && size <= BRY_CONFIG_LIMIT) {
    screenStorageError = "config_record_write";
    ok = screenPersistConfig(&screenStore, doc);
  }
  if (ok) screenStorageError = "";
  xSemaphoreGive(screenStoreMutex);
  return ok;
}
static const char *screenApplySettings(JsonObjectConst params,
                                       JsonObject data) {
  if ((screenTail + 1) % 12 == screenHead)
    return "busy";
  bool durable = screenStoreAvailable;
  if (screenBridge.extended && !durable)
    return "storage_error";
  DynamicJsonDocument previous(1024);
  DynamicJsonDocument doc(1024);
  if (previous.capacity() != 1024 || doc.capacity() != 1024 ||
      !previous.set(screenSavedSettings))
    return "busy";
  String token = "iotcloud-settings-" + String(millis());
  doc["client_token"] = token;
  doc["data"] = params;
  String wire;
  if (!screenBuildDelivery(screenRoot() + "/mqtt/service/set", doc, wire))
    return "busy";
  if (durable) {
    for (JsonPairConst pair : params)
      screenSavedSettings[pair.key()] = pair.value();
    if (screenSavedSettings.overflowed() || !screenSave()) {
      screenSavedSettings.set(previous);
      return "storage_error";
    }
  }
  /* The single owner reserved this queue slot before persistence. Move the
   * already complete wire buffer, so no allocation can fail after the save. */
  screenQueue[screenTail].wire = std::move(wire);
  screenQueue[screenTail].at = millis() + 1;
  screenTail = (screenTail + 1) % 12;
  screenPendingToken = std::move(token);
  screenPendingAt = millis();
  screenPendingCode = -1;
  invalidateBusinessStatusCache();
  data["token"] = screenPendingToken;
  return "accepted";
}
static void screenRestoreSettings() {
  if (!screenBridge.extended || screenSettingsSession == screenBridge.session ||
      screenImage.active)
    return;
  if (screenSavedSettings.size()) {
    if ((screenTail + 1) % 12 == screenHead)
      return;
    DynamicJsonDocument doc(1536);
    String token = "iotcloud-restore-" + String(millis());
    doc["client_token"] = token;
    doc["data"] = screenSavedSettings;
    if (!screenDelivery(screenRoot() + "/mqtt/service/set", doc, 1))
      return;
    screenPendingToken = token;
    screenPendingAt = millis();
    screenPendingCode = -1;
    invalidateBusinessStatusCache();
  }
  screenSettingsSession = screenBridge.session;
}
static void screenLoad() {
  screenStartupAuto = true; // Records written before this option keep the new default.
  scene_defaults(&screenScene);
  appearance_default(&screenAppearance);
  display_policy_default(&screenDisplayPolicy);
  screenSchedule.timezone = 480;
  screenSchedule.wake = REM_RESPECT_DND;
  if (!screenStore.owned)
    return;
  size_t used = 0;
  if (bry_store_load(&screenStore, nullptr, 0, &used)) {
    /* Allocate the validated record size, not a permanent 48 KiB maximum.
     * Boot-time Wi-Fi leaves limited contiguous memory for the JSON pool. */
    std::unique_ptr<uint8_t[]> bytes(new (std::nothrow) uint8_t[used]);
    screenConfigLoadError = "allocation_or_read_failed";
    if (bytes && bry_store_load(&screenStore, bytes.get(), used, &used)) {
      DynamicJsonDocument doc(min((size_t)40960, used * 2 + 4096));
      /* Const input owns strings in doc; copied config must outlive bytes. */
      auto error = deserializeMsgPack(doc, (const uint8_t *)bytes.get(), used);
      screenConfigLoadError = error ? error.c_str() : "invalid_schema";
      if (!error && doc["schema"] == 2) {
        screenConfigLoadError = "";
        if(!screenConfigStartupAuto(doc.as<JsonObjectConst>(),screenStartupAuto))
          screenConfigLoadError="invalid_startup_policy";
        size_t n;
        {
          std::unique_ptr<uint8_t[]> layout(new (std::nothrow) uint8_t[SCENE_BYTES]);
          if (!layout ||
              !screenUnbase64(doc["layout"], layout.get(), SCENE_BYTES, n) ||
              !scene_decode(&screenScene, layout.get(), n))
            screenConfigLoadError = "invalid_layout";
        }
        uint8_t policyBytes[DISPLAY_POLICY_BYTES];
        if (!doc["policy"].isNull() &&
            (!screenUnbase64(doc["policy"], policyBytes, sizeof(policyBytes), n) ||
             !display_policy_decode(&screenDisplayPolicy, policyBytes, n)))
          screenConfigLoadError = "invalid_display_policy";
        uint8_t appearanceBytes[APPEARANCE_BYTES];
        if(!doc["appearance"].isNull()&&
           (!screenUnbase64(doc["appearance"],appearanceBytes,sizeof(appearanceBytes),n)||
            !appearance_decode(&screenAppearance,appearanceBytes,n)))screenConfigLoadError="invalid_appearance";
        screenWeatherConfig.set(doc["weather"]);
        if(screenWeatherConfig.overflowed()){screenWeatherConfig.clear();screenConfigLoadError="invalid_weather_config";}
        screenSavedSettings.set(doc["settings"]);
        int timezone = doc["timezone"] | 480;
        screenSchedule.timezone =
            timezone >= -720 && timezone <= 840 ? timezone : 480;
        unsigned wake = doc["wake"] | 0;
        screenSchedule.wake = wake <= 2 ? wake : 0;
        unsigned index = 0;
        for (JsonObjectConst o : doc["rules"].as<JsonArrayConst>()) {
          if (index >= REMINDER_MAX)
            break;
          reminder_rule r = {};
          r.id = o["id"] | 0u;
          r.kind = o["kind"] | 0;
          r.epoch = o["epoch"] | 0u;
          r.minute = o["minute"] | 0;
          r.days = o["days"] | 0;
          r.wake = o["wake"] | 255;
          r.enabled = o["enabled"] | false;
          r.last_fire = o["last_fire"] | 0u;
          bry::textCopy(r.title, sizeof(r.title), o["title"] | "");
          bry::textCopy(r.text, sizeof(r.text), o["text"] | "");
          if (reminder_validate(&r))
            screenSchedule.rules[index++] = r;
        }
        index = 0;
        for (JsonObjectConst o : doc["notices"].as<JsonArrayConst>()) {
          if (index >= REMINDER_MAX)
            break;
          reminder_notice &r = screenNotices[index++];
          r.id = o["id"] | 0u;
          r.at = o["at"] | 0u;
          r.wake = o["wake"] | 0;
          r.unread = o["unread"] | false;
          bry::textCopy(r.title, sizeof(r.title), o["title"] | "");
          bry::textCopy(r.text, sizeof(r.text), o["text"] | "");
          if (r.id >= screenNoticeId)
            screenNoticeId = r.id == UINT32_MAX ? 0x80000000u : r.id + 1;
        }
      }
    }
  }
  uint8_t h[BRY_IMAGE_HEADER_BYTES];
  if (screenReadStore(nullptr, BRY_IMAGE_HEADER, h, sizeof(h)) &&
      bry_image_header_decode(h, BRY_IMAGE_LIMIT, screenImage.md5,
                              screenImage.sha, &screenImage.features)) {
    screenImage.valid = true;
    screenImage.size = scene_u32(h + 8);
    screenImage.state = "artifact_ready";
  }
}
static bool screenBridgeSend(void *, const char *p, size_t n) {
  return screenBusinessSend(p, n);
}
static void screenExtension(void *, const void *data, size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  if(n&&p[0]==SCREEN_MSG_FONT_STATUS){screenFontReceive(p,n);return;}
  if(n&&p[0]==SCREEN_MSG_WALLPAPER_STATUS){screenWallpaperReceive(p,n);return;}
  if(n==7&&p[0]==SC_MSG_ACK&&p[1]==SCREEN_MSG_APPEARANCE) {
    if(scene_u32(p+3)==screenAppearance.revision) {
      screenAppearanceError=p[2]?"mcu_rejected":"";
      if(!p[2])screenAppearanceAck=scene_u32(p+3);
    }
    return;
  }
  if(n&&p[0]==SCREEN_MSG_PAGE_STATUS){screenPageReceive(p,n);return;}
  if (n && p[0] == SCREEN_MSG_DIAG_REPLY){screenDiagReceive(p,n);return;}
  if (n && p[0] == 0x2a) { screenResourceReceive(p, n); return; }
  if (n && p[0] == 0x2c) { screenModuleReceive(p, n); return; }
  if (n && p[0] == SCREEN_MSG_BACKLIGHT) {
    screenBrightnessReceive(p, n);
    return;
  }
  if (n == 7 && p[0] == SC_MSG_ACK && p[1] == SCREEN_MSG_POLICY) {
    screenPolicyError = p[2] ? "mcu_rejected" : "";
    if (!p[2]) screenPolicyAck = scene_u32(p + 3);
    return;
  }
  if (n != 7 || p[0] != SC_MSG_ACK || !screenSyncStep)
    return;
  unsigned op = screenSyncStep == 1   ? SC_MSG_BEGIN
                : screenSyncStep == 2 ? SC_MSG_CHUNK
                                      : SC_MSG_COMMIT;
  if (p[1] != op)
    return;
  if (p[2]) {
    screenSyncError = "mcu_rejected";
    screenSyncStep = 0;
    return;
  }
  if (screenSyncStep == 1) {
    screenSyncStep = 2;
    screenSyncOffset = 0;
  } else if (screenSyncStep == 2) {
    screenSyncOffset +=
        min((uint32_t)512, screenSyncSize - screenSyncOffset);
    if (screenSyncOffset == screenSyncSize)
      screenSyncStep = 3;
  } else {
    screenSceneAck = scene_u32(p + 3);
    screenSyncStep = 0;
    screenSyncError =
        screenSceneAck == screenScene.revision ? "" : "revision_mismatch";
  }
  screenSyncDeadline = 0;
}
static void screenSync() {
  if (!(screenBridge.capabilities & SCREEN_CAP_SCENE) || screenImage.active)
    return;
  if (screenSceneSession != screenBridge.session) {
    screenSceneSession = screenBridge.session;
    screenSceneAck = 0;
    screenSyncStep = 0;
    screenSyncDeadline = screenSyncRetryAt = 0;
    screenSyncError = "";
  }
  if (screenSyncRetryAt) {
    if ((int32_t)(millis() - screenSyncRetryAt) < 0) return;
    screenSyncRetryAt = 0;
  }
  if (screenSyncDeadline) {
    if ((int32_t)(millis() - screenSyncDeadline) < 0)
      return;
    screenSyncStep = 0;
    screenSyncError = "ack_timeout";
    screenSyncDeadline = 0;
    return;
  }
  if (!screenSyncStep) {
    if (screenSceneAck == screenScene.revision)
      return;
    screenSyncStep = 1;
    screenSyncOffset = 0;
  }
  screenSyncSize = SCENE_BYTES;
  if (!screenSceneWire.prepare(screenScene)) {
    screenSyncStep = 0;
    screenSyncError = "layout_allocation";
    screenSyncRetryAt = millis() + 2000;
    return;
  }
  uint8_t p[517];
  size_t n;
  if (screenSyncStep == 1) {
    p[0] = SC_MSG_BEGIN;
    scene_put32(p + 1, screenScene.revision);
    scene_put32(p + 5, screenSyncSize);
    scene_put32(p + 9, screenSceneWire.crc());
    n = 13;
  } else if (screenSyncStep == 2) {
    p[0] = SC_MSG_CHUNK;
    scene_put32(p + 1, screenSyncOffset);
    n = min((uint32_t)512, screenSyncSize - screenSyncOffset);
    if (!screenSceneWire.read(screenSyncOffset, p + 5, n)) return;
    n += 5;
  } else {
    p[0] = SC_MSG_COMMIT;
    scene_put32(p + 1, screenScene.revision);
    n = 5;
  }
  if (bry_bridge_send(&screenBridge, p, n, SCREEN_CAP_SCENE))
    screenSyncDeadline = millis() + 2000;
}
static bool screenNoticeWire(const reminder_notice &r) {
  uint8_t p[204] = {SC_MSG_REMINDER};
  scene_put32(p + 1, r.id);
  scene_put32(p + 5, r.at);
  p[9] = r.wake;
  p[10] = r.unread;
  memcpy(p + 11, r.title, 65);
  memcpy(p + 76, r.text, 128);
  return bry_bridge_send(&screenBridge, p, sizeof(p), SCREEN_CAP_SERVICES);
}
static bool screenNotice(uint32_t id, const char *title, const char *text,
                         uint8_t wake, bool unread = true) {
  if (!id || wake > 2 || strlen(title) > 64 || strlen(text) > 127 ||
      !scene_utf8(title, 65) || !scene_utf8(text, 129))
    return false;
  unsigned slot = REMINDER_MAX;
  for (unsigned i = 0; i < REMINDER_MAX; i++) {
    if (screenNotices[i].id == id) {
      slot = i;
      break;
    }
    if (slot == REMINDER_MAX && !screenNotices[i].unread)
      slot = i;
  }
  if (slot == REMINDER_MAX)
    return false;
  reminder_notice previous = screenNotices[slot], &r = screenNotices[slot];
  r = {};
  r.id = id;
  r.at = time(nullptr);
  r.wake = wake;
  r.unread = unread;
  bry::textCopy(r.title, sizeof(r.title), title);
  bry::textCopy(r.text, sizeof(r.text), text);
  if (!screenSave()) {
    r = previous;
    return false;
  }
  screenNoticeWire(r);
  return true;
}
#include "screen_guard.inc"
#include "screen_startup.inc"
#include "screen_module.inc"
static void screenServicesStatus(JsonObject s) {
  s["extension_supported"] = screenBridge.extended;
  s["capabilities"] = screenBridge.capabilities;
  s["layout_revision"] = screenScene.revision;
  s["mcu_layout_revision"] = screenSceneAck;
  s["layout_sync"] = screenSyncStep                           ? "syncing"
                     : screenSceneAck == screenScene.revision ? "applied"
                                                              : "waiting";
  s["layout_error"] = screenSyncError;
  s["layout_cache_bytes"] = screenSceneWire.size();
  s["layout_cache_storage"] = screenSceneWire.storage();
  s["storage"] = screenStoreAvailable
                     ? (screenStore.owned ? "owned" : "available_blank")
                     : "unsupported_or_occupied";
  s["storage_error"] = screenStorageError;
  s["config_load_error"] = screenConfigLoadError;
  s["config_generation"] = screenStore.generation;
  s["flash_configured_size"] = screenFlashConfigured;
  s["flash_physical_size"] = screenFlashPhysical;
  s["weather_available"] = screenWeather.valid && !screenWeather.expired;
  s["weather_provider"] = screenWeather.provider;
  screenBrightnessStatus(s.createNestedObject("brightness"));
  screenResourceStatus(s.createNestedObject("resources"));
  screenAppearanceGet(s.createNestedObject("appearance"));
  screenModuleStatus(s.createNestedObject("module"));
  screenPageStatus(s.createNestedObject("page"));
  screenPolicyGet(s.createNestedObject("display"));
  unsigned unread = 0;
  for (auto &r : screenNotices)
    unread += r.unread;
  s["unread_reminders"] = unread;
  JsonObject ota = s.createNestedObject("ota");
  ota["state"] = screenImage.state;
  ota["sent"] = screenImage.offset;
  ota["size"] = screenImage.size;
  ota["sha256"] = screenHex(screenImage.sha, 32);
  ota["recovery_image"] = bool(screenImage.features & 1);
  ota["installable"] = screenGuardAvailable() && screenGuard.activation;
  ota["gate"] = !screenGuardAvailable() ? "factory_bootstrap_required" :
                !screenGuard.activation ? "staging_only_mcu" : "ready";
  screenGuardStatus(s.createNestedObject("recovery"), false);
  screenStartupGet(s.createNestedObject("startup"));
  screenClockStatus(s.createNestedObject("clock"), screenSchedule.timezone);
}
static void screenServicesInit() {
  screenStoreMutex = xSemaphoreCreateMutex();
  screenStoreAvailable = screenStoreMutex && screenOpenStorage();
  screenLoad();
  screenBridge.send = screenBridgeSend;
  screenBridge.extension = screenExtension;
  bry_bridge_init(&screenBridge);
}
// clang-format off: includes share one owner and have declaration dependencies.
#include "screen_ota.inc"
#include "screen_appearance.inc"
#include "screen_font.inc"
#include "display_policy.inc"
#include "weather_net.inc"
#include "screen_page.inc"
#include "screen_diagnostics.inc"
#include "screen_commands.inc"
// clang-format on
static void screenServicesPoll(uint32_t now) {
  screenLayoutUpload.expire(now);
  screenModulePowerPoll(now);
  screenAppearancePoll();
  screenWallpaperPoll(now);screenFontPoll(now);
  if (!strcmp(screenGuard.state, "pending") &&
      (int32_t)(now - screenGuard.deadline) >= 0) {
    screenGuard.state = "timeout";
    invalidateBusinessStatusCache();
  }
  if (screenImage.active && (int32_t)(now - screenImage.deadline) >= 0) {
    screenImage.active = false;
    screenImage.pending = false;
    screenImage.state = "timeout";
  }
  screenStartupPoll(now,screenImage.active||screenUploadBusy||screenFontBusy()||screenWallpaper.active||screenWallpaper.pending);
  screenImportArtifact();
  screenSync();
  screenWeatherPoll(now);
  screenPolicyPoll();
  screenRestoreSettings();
  if (now - screenServicesAt < 1000 || screenImage.active)
    return;
  screenServicesAt = now;
  invalidateBusinessStatusCache();
  uint32_t cursor = screenSchedule.cursor;
  bool started = screenSchedule.started;
  uint32_t epoch = time(nullptr), ids[REMINDER_MAX];
  uint32_t previous[REMINDER_MAX];
  for (unsigned i = 0; i < REMINDER_MAX; i++)
    previous[i] = screenSchedule.rules[i].last_fire;
  unsigned count =
      reminder_due(&screenSchedule, epoch, epoch >= 1700000000u, ids);
  uint32_t due[REMINDER_MAX];
  for (unsigned i = 0; i < REMINDER_MAX; i++) {
    due[i] = screenSchedule.rules[i].last_fire;
    screenSchedule.rules[i].last_fire = previous[i];
  }
  for (unsigned i = 0; i < count; i++)
    for (auto &r : screenSchedule.rules)
      if (r.id == ids[i]) {
        unsigned ruleIndex = &r - screenSchedule.rules;
        r.last_fire = due[ruleIndex];
        if (!screenNotice(r.id, r.title, r.text,
                          r.wake == REM_INHERIT ? screenSchedule.wake
                                                : r.wake)) {
          /* Retry the same time window; already persisted last_fire suppresses
           * duplicates. */
          screenSchedule.cursor = cursor;
          screenSchedule.started = started;
          for (unsigned j = 0; j < REMINDER_MAX; j++)
            if (screenSchedule.rules[j].id == r.id)
              screenSchedule.rules[j].last_fire = previous[j];
        }
      }
  if (screenBridge.capabilities & SCREEN_CAP_SERVICES) {
    if (epoch >= 1700000000u) {
      uint8_t p[7] = {SC_MSG_CLOCK};
      scene_put32(p + 1, epoch);
      p[5] = screenSchedule.timezone;
      p[6] = screenSchedule.timezone >> 8;
      bry_bridge_send(&screenBridge, p, sizeof(p), SCREEN_CAP_SERVICES);
    }
    static uint32_t session;
    static unsigned noticeAt = 0;
    if (session != screenBridge.session) {
      session = screenBridge.session;
      noticeAt = 0;
    }
    if (noticeAt < REMINDER_MAX) {
      if (screenNotices[noticeAt].id) {
        reminder_notice replay = screenNotices[noticeAt];
        replay.wake = REM_UNREAD_ONLY;
        screenNoticeWire(replay);
      }
      noticeAt++;
    }
  }
}

static bool screenExtendedLine(const uint8_t *p, size_t n) {
  if ((n >= 12 && !memcmp(p, "screen_caps ", 12)) ||
      (n >= 11 && !memcmp(p, "screen_ext ", 11))) {
    bry_bridge_feed(&screenBridge, p, n);
    bry_bridge_feed(&screenBridge, "\r\n", 2);
    return true;
  }
  if (n == 8 && !memcmp(p, "version?", 8)) {
    bry_bridge_reset(&screenBridge);
    screenRecovering = false;
    screenSceneAck = 0;
    screenSceneSession = 0;
    screenSettingsSession = 0;
  }
  return false;
}

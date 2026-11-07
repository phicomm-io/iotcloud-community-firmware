/* Internal boot-flash export runs independently of the UI capability session. */
#include "protocol/guard.h"
struct ScreenGuard {
  bool activation = false;
  uint32_t exported = 0;
  uint32_t session = 0, sequence = 0, seen = 0, remaining = 0;
  uint32_t inheritedControl = 0, inheritedReset = 0, inheritedClock = 0,
           resetStatus = 0;
  unsigned mode = 0, operation = 0, result = 0;
  uint32_t request = 0, offset = 0, deadline = 0;
  unsigned length = 0;
  uint8_t bytes[SCREEN_BOOT_CHUNK] = {};
  char imageMd5[33] = {};
  uint32_t diagnostic[20] = {}, diagnosticSeen = 0;
  unsigned diagnosticVersion = 0;
  bool hasDiagnostic = false;
  const char *state = "unavailable";
};
static ScreenGuard screenGuard;
static bool screenGuardAvailable() {
  return screenGuard.session && uint32_t(millis() - screenGuard.seen) < 15000;
}
static bool screenGuardAnnounce(const String &arg) {
  unsigned version, session, sequence, mode, remaining, control, reset, clock, status;
  char extra;
  if (sscanf(arg.c_str(), "%u %8x %u %u %u %8x %8x %8x %8x%c", &version, &session,
             &sequence, &mode, &remaining, &control, &reset, &clock, &status,
             &extra) != 9 || version != 3 || !session || mode > 3 || remaining > 30000)
    return true;
  if (session != screenGuard.session) {
    screenGuard = {};
    screenGuard.session = session;
    screenGuard.state = "ready";
  }
  screenGuard.sequence = max(screenGuard.sequence, (uint32_t)sequence);
  screenGuard.seen = millis();
  screenGuard.mode = mode;
  screenGuard.activation = true;
  screenGuard.remaining = remaining;
  screenGuard.inheritedControl = control;
  screenGuard.inheritedReset = reset;
  screenGuard.inheritedClock = clock;
  screenGuard.resetStatus = status;
  screenRecovering = mode != 0;
  invalidateBusinessStatusCache();
  return true;
}
static bool screenGuardImageAnnounce(const String &arg) {
  if (arg.length() == 34 && arg.startsWith("1 ")) {
    bool ok = true;
    for (unsigned i = 2; i < 34; i++) {
      char c = arg.c_str()[i];
      ok = ok && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
    if (ok) {
      memcpy(screenGuard.imageMd5, arg.c_str() + 2, 32);
      screenGuard.imageMd5[32] = 0;
      if (screenGuardAvailable() &&
          (!strcmp(screenImage.state, "factory_installing") ||
           !strcmp(screenImage.state, "installing")) &&
          screenHex(screenImage.md5, 16) == screenGuard.imageMd5)
        screenImage.state = "installed";
      invalidateBusinessStatusCache();
    }
  }
  return true;
}
static bool screenGuardLine(const uint8_t *p, size_t n) {
  uint8_t reply[11 + SCREEN_BOOT_CHUNK];
  size_t size = sizeof(reply);
  uint32_t session, sequence;
  if (!screenGuard.session || !screen_link_decode((const char *)p, n, &session,
          &sequence, reply, &size) || session != screenGuard.session || !size)
    return false;
  if (reply[0] == SCREEN_GUARD_DIAGNOSTIC) {
    if ((size == SCREEN_GUARD_DIAGNOSTIC_V3_BYTES && reply[1] == 3) &&
        scene_u32(reply + 2) <= 6 && scene_u32(reply + 6) >= 1 &&
        scene_u32(reply + 6) <= 17) {
      for (unsigned i = 0; i < 20; i++)
        screenGuard.diagnostic[i] = i < (size - 2) / 4 ?
            scene_u32(reply + 2 + i * 4) : 0;
      screenGuard.diagnosticVersion = reply[1];
      screenGuard.hasDiagnostic = true;
      screenGuard.diagnosticSeen = millis();
      invalidateBusinessStatusCache();
    }
    return true;
  }
  if (reply[0] != SCREEN_GUARD_REPLY)
    return false;
  if (strcmp(screenGuard.state, "pending") || sequence != screenGuard.request)
    return true;
  if (size < 11 || reply[1] != screenGuard.operation ||
      scene_u32(reply + 3) != screenGuard.offset ||
      scene_u32(reply + 7) != SCREEN_BOOT_BYTES || reply[2] > GUARD_IO ||
      (reply[2] == GUARD_OK &&
       size != 11 + (screenGuard.operation == GUARD_READ_BOOT ? screenGuard.length : 0)) ||
      (reply[2] != GUARD_OK && size != 11)) {
    screenGuard.state = "invalid_response";
  } else {
    screenGuard.result = reply[2];
    screenGuard.state = reply[2] == GUARD_OK ? "complete" : "mcu_rejected";
    if (reply[2] == GUARD_OK && screenGuard.operation == GUARD_READ_BOOT) {
      memcpy(screenGuard.bytes, reply + 11, screenGuard.length);
      if (screenGuard.offset == screenGuard.exported)
        screenGuard.exported += screenGuard.length;
    }
  }
  invalidateBusinessStatusCache();
  return true;
}
static const char *screenGuardRequest(unsigned op, uint32_t offset, unsigned n,
                                       JsonObject data) {
  if (!screenGuardAvailable())
    return "not_supported";
  if (screenImage.active || !strcmp(screenGuard.state, "pending"))
    return "busy";
  if (screenGuard.sequence == UINT32_MAX)
    return "not_ready";
  uint8_t p[8] = {SCREEN_GUARD_REQUEST, (uint8_t)op};
  scene_put32(p + 2, offset);
  p[6] = n;
  p[7] = n >> 8;
  char wire[64];
  uint32_t request = ++screenGuard.sequence;
  size_t used = screen_link_encode(wire, sizeof(wire), screenGuard.session,
                                   request, p, sizeof(p));
  if (!used || screenUart.write((const uint8_t *)wire, used) != used)
    return "busy";
  screenGuard.operation = op;
  screenGuard.offset = offset;
  screenGuard.length = n;
  screenGuard.request = request;
  screenGuard.state = "pending";
  /* Installation rehashes the full NOR through bounded GPIO SPI. */
  screenGuard.deadline = millis() + (op == GUARD_INSTALL ? 60000 : 3000);
  data["request"] = request;
  data["session"] = screenGuard.session;
  return "accepted";
}
static void screenGuardStatus(JsonObject data, bool bytes) {
  data["available"] = screenGuardAvailable();
  data["diagnostic_max_version"] = 3;
  data["session"] = screenGuard.session;
  data["mode"] = !screenGuardAvailable() ? "unavailable" : screenGuard.mode == 0 ? "normal" : screenGuard.mode == 1 ? "window" : screenGuard.mode == 2 ? "held" : "fault_recovery";
  uint32_t age = millis() - screenGuard.seen;
  data["window_ms"] = screenGuardAvailable() && screenGuard.remaining > age ? screenGuard.remaining - age : 0;
  data["ota_activation"] = screenGuardAvailable() && screenGuard.activation;
  data["boot_exported"] = screenGuard.exported;
  data["image_md5"] = screenGuard.imageMd5;
  data["state"] = screenGuard.state;
  data["request"] = screenGuard.request;
  data["result"] = screenGuard.result;
  data["offset"] = screenGuard.offset;
  data["total"] = SCREEN_BOOT_BYTES;
  JsonObject wdt = data.createNestedObject("inherited_wdt");
  wdt["control"] = screenGuard.inheritedControl;
  wdt["reset_value"] = screenGuard.inheritedReset;
  wdt["clock_select"] = screenGuard.inheritedClock;
  wdt["reset_status"] = screenGuard.resetStatus;
  JsonObject diag = data.createNestedObject("diagnostic");
  bool present = screenGuardAvailable() && screenGuard.hasDiagnostic &&
      uint32_t(millis() - screenGuard.diagnosticSeen) < 15000;
  diag["available"] = present;
  if (present) {
    static const char *const names[] = {"reason", "stage", "previous_stage",
      "ipsr", "cfsr", "hfsr", "mmfar", "bfar", "exception_lr",
      "exception_msp", "rtc_status", "sdram_error", "uptime_ms",
      "fault_pc", "stacked_lr", "stacked_xpsr", "control_actual",
      "control_expected", "control_address", "sensor_step"};
    diag["version"] = screenGuard.diagnosticVersion;
    unsigned count = 20u;
    for (unsigned i = 0; i < count; i++)
      diag[names[i]] = screenGuard.diagnostic[i];
  }
  if (bytes && screenGuard.operation == GUARD_READ_BOOT &&
      !strcmp(screenGuard.state, "complete")) {
    data["length"] = screenGuard.length;
    data["bytes"] = screenBase64(screenGuard.bytes, screenGuard.length);
    data["crc32"] = scene_crc32(screenGuard.bytes, screenGuard.length);
  }
}

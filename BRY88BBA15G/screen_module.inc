/* PD12 readback, bounded low lease, and an acknowledgement per request. */
#include "protocol/module_control.h"
#include <atomic>
/* HTTP tasks request a lease; only the application owner writes the UART. */
static std::atomic<bool> screenPowerWanted{false}, screenPowerReady{false},
                         screenPowerFailed{false};
static bool screenPowerLeased;
static uint32_t screenPowerSession, screenPowerRequest, screenPowerRenewAt;
static struct {
  uint32_t session, at, request, deadline, remaining, frames, transitions;
  uint8_t flags;
  bool received;
  const char *state = "idle";
} screenModule;
static bool screenModuleWrite(bool high, uint32_t duration) {
  if (screenModule.session != screenBridge.session) {
    screenModule = {}; screenModule.session = screenBridge.session;
  }
  if (screenModule.request == UINT32_MAX) return false;
  uint32_t request = screenModule.request + 1;
  uint8_t payload[SCREEN_MODULE_REQUEST_BYTES] = {SCREEN_MSG_MODULE_REQUEST, 1,
      (uint8_t)(high ? SCREEN_MODULE_HIGH : SCREEN_MODULE_LOW), 0};
  scene_put32(payload + 4, request); scene_put32(payload + 8, duration);
  if (!bry_bridge_send(&screenBridge, payload, sizeof(payload), SCREEN_CAP_MODULE_CONTROL)) return false;
  screenModule.request = request; screenModule.deadline = millis() + 3000;
  screenModule.state = "pending";
  return true;
}
static void screenModulePowerPoll(uint32_t now) {
  bool wanted = screenPowerWanted.load();
  if (!wanted) {
    screenPowerReady = false;
    if (screenPowerLeased) {
      /* A failed send is bounded by the MCU's 60 s lease. Never write into
       * a raw MCU OTA transfer or try to wake its recovery-only service. */
      if (!screenImage.active && !screenRecovering && screenGuardAvailable() && !screenGuard.mode)
        screenModuleWrite(true, 0);
      screenPowerLeased = false;
    }
    screenPowerRequest = 0;
    return;
  }
  /* Recovery owns PD12 in internal SRAM and already keeps it low. Its
   * minimal UART service cannot answer a normal-business module request;
   * stale capabilities must never block uploading a recovery image. */
  if (!screenPowerLeased && screenRecovering && screenGuardAvailable() && screenGuard.mode) {
    screenPowerReady = true;
    return;
  }
  bool supported = screenBridge.extended &&
                   (screenBridge.capabilities & SCREEN_CAP_MODULE_CONTROL);
  if (!supported) {
    if (screenPowerLeased) screenPowerFailed = true;
    else screenPowerReady = true; // Preserve the factory-peer OTA path.
    return;
  }
  if (screenPowerFailed) return;
  if (screenImage.active || screenRecovering || !screenGuardAvailable() || screenGuard.mode) {
    screenPowerReady = false; screenPowerFailed = true; return;
  }
  if (screenPowerLeased && screenPowerSession != screenBridge.session) {
    screenPowerReady = false; screenPowerFailed = true; return;
  }
  if (screenPowerRequest) {
    if (screenModule.request != screenPowerRequest ||
        !strcmp(screenModule.state, "mcu_rejected") ||
        (int32_t)(now - screenModule.deadline) >= 0) {
      screenPowerReady = false; screenPowerFailed = true; return;
    }
    if (!strcmp(screenModule.state, "complete")) {
      if ((screenModule.flags & 7) != 5 || !screenModule.remaining) {
        screenPowerReady = false; screenPowerFailed = true; return;
      }
      screenPowerReady = true;
      screenPowerRequest = 0; screenPowerRenewAt = now + 15000;
    }
    return;
  }
  if (!screenPowerLeased || (int32_t)(now - screenPowerRenewAt) >= 0) {
    if (!screenModuleWrite(false, SCREEN_MODULE_MAX_MS)) {
      screenPowerReady = false; screenPowerFailed = true; return;
    }
    screenPowerLeased = true; screenPowerSession = screenBridge.session;
    screenPowerRequest = screenModule.request;
  }
}
static void screenModulePowerEnd() {
  screenPowerWanted = false;
}
static bool screenModulePowerBegin() {
  bool expected = false;
  if (!screenPowerWanted.compare_exchange_strong(expected, true)) return false;
  screenPowerReady = false; screenPowerFailed = false;
  uint32_t start = millis();
  while (!screenPowerReady.load()) {
    if (screenPowerFailed.load() || uint32_t(millis() - start) >= 4000) {
      screenModulePowerEnd(); return false;
    }
    delay(1);
  }
  return true;
}
static bool screenModulePowerHealthy() {
  return screenPowerWanted.load() && screenPowerReady.load() && !screenPowerFailed.load();
}
static void screenModuleReceive(const uint8_t *p, size_t n) {
  if (n != SCREEN_MODULE_STATUS_BYTES || p[0] != SCREEN_MSG_MODULE_STATUS ||
      p[1] != 1 || p[2] > 2 || p[3] > 7 ||
      !(screenBridge.capabilities & SCREEN_CAP_MODULE_CONTROL) ||
      scene_u32(p + 8) > SCREEN_MODULE_MAX_MS ||
      bool(p[3] & 4) != bool(scene_u32(p + 8))) return;
  uint32_t request = scene_u32(p + 4);
  if (screenModule.session != screenBridge.session) screenModule = {};
  screenModule.session = screenBridge.session; screenModule.at = millis();
  screenModule.flags = p[3]; screenModule.remaining = scene_u32(p + 8);
  screenModule.frames = scene_u32(p + 12); screenModule.transitions = scene_u32(p + 16);
  screenModule.received = true;
  if (request && request == screenModule.request)
    screenModule.state = p[2] ? "mcu_rejected" : "complete";
  invalidateBusinessStatusCache();
}
static void screenModuleStatus(JsonObject out) {
  bool supported = screenBridge.extended &&
                   (screenBridge.capabilities & SCREEN_CAP_MODULE_CONTROL);
  bool fresh = supported && screenModule.received && !screenRecovering &&
               screenModule.session == screenBridge.session &&
               uint32_t(millis() - screenModule.at) < 5000;
  if (!strcmp(screenModule.state ? screenModule.state : "idle", "pending") &&
      (screenModule.session != screenBridge.session ||
       (int32_t)(millis() - screenModule.deadline) >= 0))
    screenModule.state = "timeout";
  out["supported"] = supported; out["available"] = fresh;
  out["pin"] = "PD12"; out["request"] = screenModule.request;
  out["state"] = screenModule.state ? screenModule.state : "idle";
  out["set_high"] = nullptr; out["initialized"] = nullptr;
  out["remaining_ms"] = nullptr; out["valid_frames"] = nullptr;
  out["transitions"] = nullptr; out["age_ms"] = nullptr;
  if (fresh) {
    uint32_t age = millis() - screenModule.at;
    out["initialized"] = bool(screenModule.flags & 1);
    out["set_high"] = bool(screenModule.flags & 2);
    out["remaining_ms"] = screenModule.remaining > age ? screenModule.remaining - age : 0;
    out["valid_frames"] = screenModule.frames; out["transitions"] = screenModule.transitions;
    out["age_ms"] = age;
  }
}
static const char *screenModuleSet(JsonObjectConst p, JsonObject out) {
  if (!screenFields(p, "|set_high||duration_ms|") || !p["set_high"].is<bool>() ||
      (p.containsKey("duration_ms") && !p["duration_ms"].is<uint32_t>())) return "invalid_params";
  bool high = p["set_high"];
  uint32_t duration = p["duration_ms"] | (high ? 0u : 10000u);
  if (high ? duration != 0 : !duration || duration > SCREEN_MODULE_MAX_MS) return "invalid_params";
  if (!screenBridge.extended || !(screenBridge.capabilities & SCREEN_CAP_MODULE_CONTROL)) return "unsupported";
  if (screenPowerWanted.load() || screenPowerLeased || screenImage.active || screenRecovering || !screenGuardAvailable() || screenGuard.mode) return "busy";
  if (screenModule.session == screenBridge.session && screenModule.request == UINT32_MAX) return "not_ready";
  if (!screenModuleWrite(high, duration)) return "busy";
  out["request"] = screenModule.request; out["state"] = "pending";
  invalidateBusinessStatusCache(); return "accepted";
}

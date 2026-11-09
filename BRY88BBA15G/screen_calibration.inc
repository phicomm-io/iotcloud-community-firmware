/* G3 calibration shares the factory JSON owner. A service ACK accepts the
 * request; only the MCU snapshot reports sampling and sensor acknowledgement. */
static String screenCalibrationToken;
static uint32_t screenCalibrationAt;
static int screenCalibrationCode = -1;
static const char *screenCalibrationRequestState() {
  return !screenCalibrationToken.length() ? "idle" :
      screenCalibrationCode == 0 ? "accepted" :
      screenCalibrationCode > 0 ? "rejected" :
      uint32_t(millis()-screenCalibrationAt) < 10000 ? "pending" : "timeout";
}
static void screenCalibrationGet(JsonObject out) {
  out.set(screenSnapshot["calibration"]);
  bool supported = screenBridge.extended && screenSnapshot["calibration"]["version"] == 1;
  out["supported"] = supported;
  out["available"] = supported && !screenRecovering && !screenImage.active &&
      screenSnapshotAt && uint32_t(millis()-screenSnapshotAt) < 20000;
  out["snapshot_age_ms"] = screenSnapshotAt ? uint32_t(millis()-screenSnapshotAt) : UINT32_MAX;
  JsonObject request = out.createNestedObject("request");
  request["token"] = screenCalibrationToken;
  request["code"] = screenCalibrationCode;
  request["state"] = screenCalibrationRequestState();
}
static bool screenCalibrationResponse(const String &topic, const String &token, int code) {
  if (!topic.endsWith("/service_response/calibration") &&
      !topic.endsWith("/service_response/calibration_cancel")) return false;
  if (token == screenCalibrationToken) {
    screenCalibrationCode = code;
    invalidateBusinessStatusCache();
    screenRequestSnapshot();
  }
  return true;
}
static const char *screenCalibrationCommand(const String &action,
                                            JsonObjectConst params, JsonObject out) {
  if (action == "screen.calibration.get") {
    if (params.size()) return "invalid_params";
    screenCalibrationGet(out); return "ok";
  }
  bool start = action == "screen.calibration.start";
  if (!start && action != "screen.calibration.cancel") return "unsupported_action";
  const char *channel = params["channel"] | "";
  if (start ? (params.size()!=2 ||
      (strcmp(channel,"co2") && strcmp(channel,"tvoc")) ||
      !params["environment_confirmed"].is<bool>() ||
      !params["environment_confirmed"].as<bool>()) : params.size()!=0)
    return "invalid_params";
  screenCalibrationGet(out);
  if (!out["supported"].as<bool>()) return "unsupported_action";
  if (!out["available"].as<bool>()) return "not_ready";
  if (!strcmp(screenCalibrationRequestState(),"pending")) return "busy";
  unsigned pending = screenSnapshot["calibration"]["pending"] | 0;
  if (start && pending) return "busy";
  if (start && !screenSnapshot["calibration"][channel]["ready"].as<bool>()) return "not_ready";
  if (!start && !pending) return "not_ready";
  DynamicJsonDocument doc(384);
  String token = "iotcloud-cal-" + String(millis());
  doc["client_token"] = token;
  JsonObject data = doc.createNestedObject("data");
  if (start) { data["co2"] = !strcmp(channel,"co2") ? 1 : 0;
               data["tvoc"] = !strcmp(channel,"tvoc") ? 1 : 0; }
  if (!screenDelivery(screenRoot()+"/mqtt/service/"+
      (start ? "calibration" : "calibration_cancel"),doc,1)) return "busy";
  screenCalibrationToken = token; screenCalibrationAt = millis(); screenCalibrationCode = -1;
  invalidateBusinessStatusCache();
  out.clear(); out["token"] = token; out["state"] = "pending";
  return "accepted";
}

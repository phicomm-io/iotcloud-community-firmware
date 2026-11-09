#include <ArduinoJson.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
class String {
  std::string s;

public:
  String() = default;
  String(const char *p) : s(p) {}
  String(const std::string &p) : s(p) {}
  String(unsigned v) : s(std::to_string(v)) {}
  bool startsWith(const char *p) const {
    return s.compare(0, strlen(p), p) == 0;
  }
  bool endsWith(const char *p) const {
    return s.size() >= strlen(p) && s.compare(s.size()-strlen(p),strlen(p),p)==0;
  }
  bool operator==(const String &p) const { return s == p.s; }
  void reserve(size_t n) { s.reserve(n); }
  String &operator+=(char c) {
    s += c;
    return *this;
  }
  bool operator==(const char *p) const { return s == p; }
  bool operator!=(const char *p) const { return s != p; }
  bool operator!=(const String &p) const { return s != p.s; }
  const char *c_str() const { return s.c_str(); }
  size_t length() const { return s.size(); }
  friend String operator+(const String &a, const String &b) {
    return String(a.s + b.s);
  }
};
static void convertToJson(const String &s, JsonVariant v) { v.set(std::string(s.c_str())); }
static uint32_t ms=1000, screenSnapshotAt=1000;
static uint32_t millis() {return ms;}
static DynamicJsonDocument screenSnapshot(2048), sent(512);
static struct { bool extended=true; } screenBridge;
static struct { bool active=false; } screenImage;
static bool screenRecovering=false,sendOk=true;
static unsigned sends,snapshots;
static void invalidateBusinessStatusCache() {}
static void screenRequestSnapshot() {snapshots++;}
static String screenRoot() {return "SENSOR/LOCAL";}
static bool screenDelivery(const String &,JsonDocument &doc,unsigned) {
  sends++;sent.set(doc);return sendOk;
}
#include "screen_calibration.inc"
int main() {
  DynamicJsonDocument request(256),reply(2048);
  auto get=[&](){reply.clear();screenCalibrationGet(reply.to<JsonObject>());};
  get();assert(!reply["supported"].as<bool>());
  screenSnapshot["calibration"]["version"]=1;
  screenSnapshot["calibration"]["pending"]=0;
  screenSnapshot["calibration"]["co2"]["ready"]=true;
  auto p=request.to<JsonObject>();p["channel"]="co2";p["environment_confirmed"]=true;
  auto call=[&](const char *action){reply.clear();return screenCalibrationCommand(action,p,reply.to<JsonObject>());};
  p["environment_confirmed"]=1;
  assert(!strcmp(call("screen.calibration.start"),"invalid_params")&&!sends);
  p["environment_confirmed"]=true;screenBridge.extended=false;
  assert(!strcmp(call("screen.calibration.start"),"unsupported_action")&&!sends);
  screenBridge.extended=true;ms=22000;
  assert(!strcmp(call("screen.calibration.start"),"not_ready")&&!sends);
  ms=1001;
  assert(!strcmp(call("screen.calibration.start"),"accepted")&&sends==1);
  assert(sent["data"]["co2"]==1&&sent["data"]["tvoc"]==0);
  assert(!strcmp(call("screen.calibration.start"),"busy")&&sends==1);
  get();assert(reply["request"]["state"]=="pending");
  assert(screenCalibrationResponse("SENSOR/LOCAL/mqtt/service_response/calibration",screenCalibrationToken,0));
  assert(snapshots==1);get();assert(reply["request"]["state"]=="accepted");
  screenSnapshot["calibration"]["pending"]=1;
  assert(!strcmp(call("screen.calibration.start"),"busy")&&sends==1);
  request.clear();p=request.to<JsonObject>();
  assert(!strcmp(call("screen.calibration.cancel"),"accepted")&&sends==2);
  ms+=10000;get();assert(reply["request"]["state"]=="timeout"&&sends==2);
}

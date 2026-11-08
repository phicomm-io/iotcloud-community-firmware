/* Exercise the production ESP UART owner against the production MCU receiver.
 */
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <string>
#include <vector>
#include <ArduinoJson.h>
extern "C" {
#include "guard_service.h"
#include "maintenance.h"
#include "scene.h"
#include "store.h"
#include "image_header.h"
}
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
using std::min;
using std::max;
static void convertToJson(const String &s, JsonVariant v) { v.set(std::string(s.c_str())); }
using mbedtls_md5_context = MD5_CTX;
using mbedtls_sha256_context = SHA256_CTX;
static void mbedtls_md5_init(MD5_CTX *) {}
static void mbedtls_md5_free(MD5_CTX *) {}
static int mbedtls_md5_starts_ret(MD5_CTX *p) {
  return MD5_Init(p) == 1 ? 0 : -1;
}
static int mbedtls_md5_update_ret(MD5_CTX *p, const void *d, size_t n) {
  return MD5_Update(p, d, n) == 1 ? 0 : -1;
}
static int mbedtls_md5_finish_ret(MD5_CTX *p, uint8_t *d) {
  return MD5_Final(d, p) == 1 ? 0 : -1;
}
static void mbedtls_sha256_init(SHA256_CTX *) {}
static void mbedtls_sha256_free(SHA256_CTX *) {}
static int mbedtls_sha256_starts_ret(SHA256_CTX *p, int) {
  return SHA256_Init(p) == 1 ? 0 : -1;
}
static int mbedtls_sha256_update_ret(SHA256_CTX *p, const void *d, size_t n) {
  return SHA256_Update(p, d, n) == 1 ? 0 : -1;
}
static int mbedtls_sha256_finish_ret(SHA256_CTX *p, uint8_t *d) {
  return SHA256_Final(d, p) == 1 ? 0 : -1;
}
static uint32_t testMillis = 100;
static bool slowNor;
static uint32_t millis() { return testMillis; }
static void delay(unsigned) {}
static void invalidateBusinessStatusCache() {}
static struct {
  void println(const char *) {}
  void printf(const char *, ...) {}
} Serial;
#define portMUX_TYPE unsigned
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
struct ScreenImage {
  bool valid = false, active = false, pending = false, bootstrap = false;
  uint32_t features = 0;
  uint32_t size = 0, offset = 0, chunk = 0, deadline = 0;
  uint8_t md5[16] = {}, sha[32] = {};
  const char *state = "empty";
};
static ScreenImage screenImage;
static bool screenRecovering;
static std::vector<uint8_t> image(BRY_IMAGE_LIMIT), nor(0x68000);
static std::string toMcu, toEsp;
static unsigned business;
static bool screenReadStore(void *, uint32_t a, void *p, size_t n) {
  if (a < BRY_IMAGE_START || a - BRY_IMAGE_START + n > image.size())
    return false;
  memcpy(p, image.data() + a - BRY_IMAGE_START, n);
  return true;
}
static bool screenSend(const String &s) {
  toMcu.append(s.c_str(), s.length());
  return true;
}
static void screenReply(const String &s, bool ok = true, unsigned error = 1) {
  char suffix[40];
  snprintf(suffix, sizeof(suffix), ok ? ":OK\r\n" : ":ERROR<%04X>\r\n", error);
  screenSend("+" + s + suffix);
}
struct {
  size_t write(const uint8_t *p, size_t n) {
    toMcu.append((const char *)p, n);
    return n;
  }
} screenUart;
static size_t screenUartWrite(const uint8_t *bytes, size_t size) {
  return screenUart.write(bytes, size);
}
static bool screenGuardAnnounce(const String &);
static bool screenGuardImageAnnounce(const String &);
#include "../screen_ota.inc"
static unsigned blockedBusiness;
static String screenBase64(const uint8_t *p, size_t n) {
  char wire[440];
  size_t used = screen_link_encode(wire, sizeof(wire), 1, 1, p, n);
  assert(used);
  return std::string(wire + 40, used - 42);
}
#include "../screen_guard.inc"
static unsigned screenHead, screenTail;
static struct { String wire; } screenQueue[12];
static struct { bool active, pending; } screenWallpaper;
static bool fontBusy, lockAvailable = true;
static void *screenStoreMutex = &screenHead;
#define pdTRUE 1
static int xSemaphoreTake(void *, unsigned) { return lockAvailable; }
static void xSemaphoreGive(void *) {}
static bool screenFontBusy() { return fontBusy; }
static String screenRoot() { return "test-device"; }
static unsigned deliveries;
static bool screenDelivery(const String &topic, JsonDocument &doc, unsigned attempts) {
  assert(topic == "test-device/mqtt/service/ota" && !attempts);
  assert(doc["data"]["upgrade_method"] == 1);
  assert(doc["data"]["upgrade_info"][0]["device_type"] == "SENSOR3_MCU");
  deliveries++;
  return true;
}
static bool screenFields(JsonObjectConst params, const char *fields) {
  for (JsonPairConst pair : params) {
    std::string key = "|" + std::string(pair.key().c_str()) + "|";
    if (!strstr(fields, key.c_str())) return false;
  }
  return true;
}
#include "../screen_update_start.inc"
static void testUpdateStart() {
  DynamicJsonDocument doc(256);
  JsonObject params = doc.to<JsonObject>();
  params["mode"] = "factory-bootstrap";
  params["sha256"] = screenHex(screenImage.sha, 32);
  /* A complete application has no recovery-only feature marker. No original
   * version string or boot-export record participates in this start path. */
  assert(screenImage.features == 0 && !screenGuardAvailable());
  screenHead = 0;
  screenTail = 2;
  screenQueue[0].wire = "old-weather";
  screenQueue[1].wire = "old-clock";
  assert(!strcmp(screenUpdateStart(params), "accepted"));
  assert(deliveries == 1 && screenImage.active && screenImage.bootstrap);
  assert(screenHead == screenTail && !screenQueue[0].wire.length());
  screenImage.active = false;
  params["sha256"] = "wrong-digest";
  assert(!strcmp(screenUpdateStart(params), "invalid_firmware"));
  assert(deliveries == 1 && !screenImage.active);
  params["sha256"] = screenHex(screenImage.sha, 32);
  lockAvailable = false;
  assert(!strcmp(screenUpdateStart(params), "busy"));
  lockAvailable = true;
  image[8192] ^= 1;
  assert(!strcmp(screenUpdateStart(params), "invalid_firmware"));
  image[8192] ^= 1;
  fontBusy = true;
  assert(!strcmp(screenUpdateStart(params), "busy"));
  fontBusy = false;
  params["unexpected"] = true;
  assert(!strcmp(screenUpdateStart(params), "invalid_params"));
  params.remove("unexpected");
  params.remove("sha256");
  params["mode"] = "stage";
  assert(!strcmp(screenUpdateStart(params), "not_ready"));
  /* Community staging uses its maintenance endpoint, never the stock trigger. */
  assert(screenGuardAnnounce("3 12345678 0 1 30000 00000403 00001400 00003001 00000000"));
  assert(!strcmp(screenUpdateStart(params), "accepted"));
  assert(!screenImage.bootstrap && toMcu == "+screen_recover:1\r\n");
  screenImage.active = false;
  toMcu.clear();
  params["mode"] = "factory-bootstrap";
  params["sha256"] = screenHex(screenImage.sha, 32);
  assert(!strcmp(screenUpdateStart(params), "invalid_firmware"));
  screenGuard = {};
  screenRecovering = false;
}
static screen_guard guard;
static unsigned diagnosticNor=3;
static bool selectNor(void *,unsigned mode){diagnosticNor=mode;return true;}
static bool bootRead(void *, uint32_t at, void *p, size_t n) {
  assert(at + n <= SCREEN_BOOT_BYTES);
  for (size_t i = 0; i < n; i++) ((uint8_t *)p)[i] = (at + i) * 137 ^ ((at + i) >> 8);
  return true;
}
static void guardLine(void *, const char *p, size_t n) {
  assert(guard_receive(&guard, p, n, false));
}
static bool readNor(void *, uint32_t a, void *p, size_t n) {
  assert(a >= SCREEN_OTA_STAGE && a - SCREEN_OTA_STAGE + n <= nor.size());
  memcpy(p, nor.data() + a - SCREEN_OTA_STAGE, n);
  if (slowNor) testMillis += 20;
  return true;
}
static bool writeNor(void *, uint32_t a, const void *p, size_t n) {
  assert(a >= SCREEN_OTA_STAGE && a - SCREEN_OTA_STAGE + n <= nor.size());
  memcpy(nor.data() + a - SCREEN_OTA_STAGE, p, n);
  return true;
}
static bool eraseNor(void *, uint32_t a, size_t n) {
  assert(a == SCREEN_OTA_STAGE && n == nor.size());
  std::fill(nor.begin(), nor.end(), 255);
  return true;
}
static bool fromMcu(void *, const char *p, size_t n) {
  toEsp.append(p, n);
  return true;
}
static void other(void *, const char *, size_t) { business++; }
static void pump(screen_maintenance &m) {
  unsigned budget = 10000;
  while (!toMcu.empty() || !toEsp.empty()) {
    assert(budget--);
    auto input = toMcu;
    toMcu.clear();
    for (uint8_t c : input)
      maintenance_feed(&m, c, millis());
    /* Match the real foreground pump. A queued final digest must be consumed
     * before this timeout check even on the old receiver without a clock. */
    maintenance_tick(&m, millis());
    while (!toEsp.empty()) {
      if (m.receiving && screenImage.active) {
        /* Match a periodic weather/clock/reminder frame arriving after the
         * MCU accepted the packet length, before it requests the raw body. */
        const char weather[] = "screen_ext 1 weather-replay\r\n";
        auto queued = toMcu;
        assert(!screenBusinessSend(weather, sizeof(weather) - 1));
        assert(toMcu == queued);
        blockedBusiness++;
      }
      auto end = toEsp.find("\r\n");
      assert(end != std::string::npos);
      auto line = toEsp.substr(0, end);
      toEsp.erase(0, end + 2);
      auto split = line.find(' ');
      auto cmd = line.substr(0, split),
           arg = split == std::string::npos ? "" : line.substr(split + 1);
      if (cmd == "screen_ext")
        assert(screenGuardLine((const uint8_t *)line.data(), line.size()));
      else
        assert(screenOtaCommand(String(cmd), String(arg)));
    }
  }
}
int main(int argc, char **argv) {
  for (unsigned i = 0; i < image.size(); i++)
    image[i] = (i * 137) ^ 3;
  scene_put32(image.data(), 0x20010000);
  scene_put32(image.data() + 4, 0x14401);
  const char binary[] = "\0\r\nscreen_caps 1 bad\r\n+download_bin:END\r\n";
  memcpy(image.data() + 32, binary, sizeof(binary));
  if (argc > 1) {
    FILE *f = fopen(argv[1], "rb");
    assert(f);
    assert(fread(image.data(), 1, image.size(), f) == image.size());
    assert(fgetc(f) == EOF);
    fclose(f);
  }
  screenImage.valid = true;
  screenImage.size = image.size();
  assert(screenHash(screenImage.md5, screenImage.sha));
  assert(screenImageVerify());
  testUpdateStart();
  screen_maintenance m = {};
  m.send = fromMcu;
  m.line = other;
  m.update.read = readNor;
  m.update.write = writeNor;
  m.update.erase = eraseNor;
  maintenance_init(&m);
  /* An unsolicited original-MCU query must not offer an update. */
  assert(screenOtaCommand("bin_summary", "1 SV.04.00.01.00"));
  assert(toMcu == "+bin_summary:OK\r\n");
  toMcu.clear();
  for (unsigned pass = 0; pass < 3; pass++) {
    screenImage.bootstrap = pass == 2;
    slowNor = pass != 2;
    screenImage.active = true;
    screenImage.offset = 0;
    screenImage.pending = false;
    assert(maintenance_query(&m, millis()));
    pump(m);
    assert(!screenImage.active && !m.state && m.staged &&
           m.update.transfer.verified && !business);
    assert(!strcmp(screenImage.state, screenImage.bootstrap ? "factory_installing" : "staged_verified"));
    assert(image ==
           std::vector<uint8_t>(nor.begin(), nor.begin() + image.size()));
  }
  screenImage.bootstrap = false;
  assert(blockedBusiness >= 3 * BRY_IMAGE_LIMIT / 1024);
  assert(screenBusinessSend("business-resumed\r\n", 18));
  assert(toMcu == "business-resumed\r\n");
  toMcu.clear();
  assert(screenOtaCommand(">", ""));
  assert(toMcu.find("ERROR") != std::string::npos);
  toMcu.clear();
  screenImage.active = true;
  screenImage.pending = false;
  screenImage.offset = 0;
  assert(screenOtaCommand("end_ota", ""));
  assert(screenImage.active && toMcu.find("ERROR") != std::string::npos);
  toMcu.clear();
  assert(screenOtaCommand("cancel_ota", ""));
  assert(!screenOtaActive());
  toMcu.clear();
  screenImage.active = screenImage.pending = true;
  screenImage.offset = 0;
  screenImage.chunk = 1024;
  assert(screenOtaCommand(">", ""));
  assert(toMcu.size() == 1029 && toMcu.substr(0, 5) == "+bin:");
  assert(!memcmp(toMcu.data() + 5, image.data(), 1024));
  screenImage.active = false;
  toMcu.clear();
  image[1025] ^= 1;
  assert(!screenImageVerify());
  screenImportArtifact();
  /* Exercise the actual ESP recovery owner and MCU parser over the same raw
   * UART/text demultiplexer, including JSON status/base64 result production. */
  toMcu.clear();
  toEsp.clear();
  m.line = guardLine;
  guard.send = fromMcu;
  guard.read_boot = bootRead;
  guard_init(&guard, 0x12345678, 100, false);
  assert(screenOtaCommand("screen_recovery", "3 12345678 0 1 30000 00000403 00001400 00003001 00000000"));
  assert(screenGuardAvailable() && screenRecovering);
  DynamicJsonDocument status(2048);
  assert(!strcmp(screenGuardRequest(GUARD_HOLD, 0, 0, status.to<JsonObject>()), "accepted"));
  pump(m);
  assert(guard.held && !strcmp(screenGuard.state, "complete"));
  guard.nor_mode=selectNor;
  for(unsigned mode:{1u,2u,3u}) {
    assert(!strcmp(screenGuardRequest(GUARD_NOR_MODE,mode,0,status.to<JsonObject>()),"accepted"));
    pump(m);
    assert(diagnosticNor==mode&&guard.held&&!strcmp(screenGuard.state,"complete"));
  }

  /* CRC telemetry cannot complete or change a pending request, and must not
   * survive a changed session, bad CRC, wrong size/version or age expiry. */
  uint8_t telemetry[54] = {SCREEN_GUARD_DIAGNOSTIC, 1};
  const uint32_t values[13] = {6, 3, 9, 3, 0x8200, 0x40000000, 0, 0x80000000,
                             0xfffffff9, 0x2000fff0, 0, 2, 31000};
  for (unsigned i = 0; i < 13; i++) scene_put32(telemetry + 2 + i * 4, values[i]);
  char notice[256];
  size_t notice_size = screen_link_encode(notice, sizeof(notice), guard.session, 1,
                                        telemetry, sizeof(telemetry));
  screenGuard.state = "pending";
  auto acceptDiagnostic = [&](size_t length) {
    return screenGuardLine((const uint8_t *)notice, length - 2);
  };
  screenGuard.hasDiagnostic=false;
  assert(acceptDiagnostic(notice_size)&&!screenGuard.hasDiagnostic);
  uint8_t memory[SCREEN_GUARD_DIAGNOSTIC_V3_BYTES] = {};
  memcpy(memory, telemetry, sizeof(telemetry));
  scene_put32(memory+54,0x194bc);
  scene_put32(memory+58,0x16001);
  scene_put32(memory+62,0x01000000);
  memory[1] = 3;
  scene_put32(memory + 66, 0x80191b60);
  scene_put32(memory + 70, 0x80191b68);
  scene_put32(memory + 74, 0x801a44ac);
  scene_put32(memory + 78, 15);
  notice_size = screen_link_encode(notice, sizeof(notice), guard.session, 1,
                                  memory, sizeof(memory));
  assert(notice_size && acceptDiagnostic(notice_size));
  assert(!strcmp(screenGuard.state, "pending"));
  screenGuardStatus(status.to<JsonObject>(), false);
  assert(status["diagnostic"]["version"] == 3 &&
         status["diagnostic"]["fault_pc"] == 0x194bc &&
         status["diagnostic"]["control_actual"] == 0x80191b60 &&
         status["diagnostic"]["control_expected"] == 0x80191b68 &&
         status["diagnostic"]["control_address"] == 0x801a44ac &&
         status["diagnostic"]["sensor_step"] == 15);
  screenGuard.hasDiagnostic = false;
  memory[1] = 2; // A v2 header must not accept a v3-sized payload.
  notice_size = screen_link_encode(notice, sizeof(notice), guard.session, 1,
                                  memory, sizeof(memory));
  assert(acceptDiagnostic(notice_size) && !screenGuard.hasDiagnostic);
  notice_size = screen_link_encode(notice, sizeof(notice), guard.session, 1,
                                  telemetry, sizeof(telemetry));
  assert(acceptDiagnostic(notice_size) && !screenGuard.hasDiagnostic);
  screenGuard.hasDiagnostic = false;
  notice[31] = notice[31] == '0' ? '1' : '0';
  assert(!acceptDiagnostic(notice_size) && !screenGuard.hasDiagnostic);
  notice_size = screen_link_encode(notice, sizeof(notice), guard.session + 1, 1,
                                  telemetry, sizeof(telemetry));
  assert(!acceptDiagnostic(notice_size) && !screenGuard.hasDiagnostic);
  notice_size = screen_link_encode(notice, sizeof(notice), guard.session, 1,
                                  telemetry, sizeof(telemetry) - 1);
  assert(acceptDiagnostic(notice_size) && !screenGuard.hasDiagnostic);
  telemetry[1] = 2;
  notice_size = screen_link_encode(notice, sizeof(notice), guard.session, 1,
                                  telemetry, sizeof(telemetry));
  assert(acceptDiagnostic(notice_size) && !screenGuard.hasDiagnostic);
  memory[1] = 3;
  notice_size = screen_link_encode(notice, sizeof(notice), guard.session, 1,
                                  memory, sizeof(memory));
  assert(acceptDiagnostic(notice_size) && screenGuard.hasDiagnostic);
  testMillis += 15000;
  screenGuardStatus(status.to<JsonObject>(), false);
  assert(!status["diagnostic"]["available"].as<bool>());
  assert(screenOtaCommand("screen_recovery", "3 12345678 0 1 30000 00000403 00001400 00003001 00000000"));
  assert(screenGuardLine((const uint8_t *)notice, notice_size - 2));
  screenGuard.state = "complete";
  for (unsigned at = 0; at < SCREEN_BOOT_BYTES; at += SCREEN_BOOT_CHUNK) {
    assert(!strcmp(screenGuardRequest(GUARD_READ_BOOT, at, SCREEN_BOOT_CHUNK,
                                      status.to<JsonObject>()), "accepted"));
    pump(m);
    auto result = status.to<JsonObject>();
    screenGuardStatus(result, true);
    assert(result["available"] && !strcmp(result["state"], "complete"));
    uint8_t expected[SCREEN_BOOT_CHUNK];
    bootRead(nullptr, at, expected, sizeof(expected));
    assert(result["offset"].as<unsigned>() == at && result["length"] == SCREEN_BOOT_CHUNK);
    assert(result["crc32"].as<uint32_t>() == scene_crc32(expected, sizeof(expected)));
    assert(!strcmp(result["bytes"], screenBase64(expected, sizeof(expected)).c_str()));
  }
  assert(guard.exported == SCREEN_BOOT_BYTES);
  assert(!strcmp(screenGuardRequest(GUARD_RESUME, 0, 0, status.to<JsonObject>()), "accepted"));
  pump(m);
  assert(!guard.held && !strcmp(screenGuard.state, "complete"));
  assert(screenOtaCommand("screen_recovery", "3 98765432 0 1 30000 00000403 00001400 00003001 00000000"));
  assert(!screenGuard.hasDiagnostic);
  puts("production recovery owners: hold, full 80 KiB CRC/base64 export and resume passed");
  puts("production ESP owner <-> MCU: two full 421888-byte UART transfers, "
       "real MD5/SHA, NOR readback, no unsolicited update, cancel/error and "
       "corruption passed");
}

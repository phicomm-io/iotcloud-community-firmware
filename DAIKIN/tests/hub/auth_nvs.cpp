// SPDX-License-Identifier: GPL-3.0-or-later
// Production config/Preferences methods and IDF NVS; only Flash I/O is modeled.
#include "nvs_storage.hpp"
#include "hub_service.h"
#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

struct String:std::string {using std::string::string;String(const std::string&s):std::string(s){}bool reserve(size_t n){std::string::reserve(n);return true;}bool concat(const char*p,size_t n){append(p,n);return true;}size_t write(uint8_t c){push_back(char(c));return 1;}size_t write(const uint8_t*p,size_t n){append(reinterpret_cast<const char*>(p),n);return n;}};
extern "C" void esp_log_write(int, const char *, const char *, ...) {}
extern "C" uint32_t esp_log_timestamp() { return 0; }
extern "C" void _esp_error_check_failed(esp_err_t code, const char *file, int line,
                                        const char *function, const char *expression) {
  fprintf(stderr, "NVS assertion %x %s:%d %s %s\n", code, file, line, function, expression);
  abort();
}
struct PowerLoss {};
class Nor : public nvs::Partition {
public:
  std::vector<uint8_t> bytes = std::vector<uint8_t>((getenv("DAIKIN_NVS_PAGES")?atoi(getenv("DAIKIN_NVS_PAGES")):5)*4096, 255);
  long cut = -1;
  bool fail = false;
  size_t work = 0;
  const char *get_partition_name() override { return "nvs"; }
  uint32_t get_address() override { return 0; }
  uint32_t get_size() override { return bytes.size(); }
  esp_err_t read_raw(size_t offset, void *out, size_t size) override { return read(offset, out, size); }
  esp_err_t read(size_t offset, void *out, size_t size) override {
    if (offset + size > bytes.size()) return ESP_ERR_INVALID_ARG;
    memcpy(out, bytes.data() + offset, size); return ESP_OK;
  }
  esp_err_t write_raw(size_t offset, const void *in, size_t size) override { return write(offset, in, size); }
  esp_err_t write(size_t offset, const void *in, size_t size) override {
    if (fail) return ESP_ERR_FLASH_OP_FAIL;
    if (offset + size > bytes.size()) return ESP_ERR_INVALID_ARG;
    auto data = static_cast<const uint8_t *>(in);
    for (size_t i = 0; i < size; i++) {
      if (cut == 0) throw PowerLoss();
      if (cut > 0) --cut;
      assert((uint8_t(~bytes[offset + i]) & data[i]) == 0);
      bytes[offset + i] &= data[i]; ++work;
    }
    return ESP_OK;
  }
  esp_err_t erase_range(size_t offset, size_t size) override {
    if (fail) return ESP_ERR_FLASH_OP_FAIL;
    if (offset + size > bytes.size() || offset % 4096 || size % 4096) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < size; i++) {
      if (cut == 0) throw PowerLoss();
      if (cut > 0) --cut;
      bytes[offset + i] = 255; ++work;
    }
    return ESP_OK;
  }
};
static std::unique_ptr<nvs::Storage> storage;
static uint8_t space;
static Nor *flash;
static void bootNor(Nor &nor) {
  storage.reset(); flash = &nor;
  storage.reset(new(std::nothrow) nvs::Storage(&nor)); assert(storage);
  assert(storage->init(0, nor.bytes.size()/4096) == ESP_OK);
  assert(storage->createOrOpenNamespace("daikin", true, space) == ESP_OK);
}
static esp_err_t get(const char *key, nvs::ItemType type, void *out, size_t *length) {
  size_t required = 0;
  auto result = storage->getItemDataSize(space, type, key, required);
  if (result != ESP_OK) return result;
  if (!out) { *length = required; return ESP_OK; }
  if (*length < required) return ESP_ERR_NVS_INVALID_LENGTH;
  *length = required; return storage->readItem(space, type, key, out, required);
}
extern "C" esp_err_t nvs_get_str(nvs_handle_t, const char *key, char *out, size_t *length) { return get(key, nvs::ItemType::SZ, out, length); }
extern "C" esp_err_t nvs_get_blob(nvs_handle_t, const char *key, void *out, size_t *length) { return get(key, nvs::ItemType::BLOB, out, length); }
extern "C" esp_err_t nvs_set_str(nvs_handle_t, const char *key, const char *value) { return storage->writeItem(space, nvs::ItemType::SZ, key, value, strlen(value) + 1); }
extern "C" esp_err_t nvs_set_blob(nvs_handle_t, const char *key, const void *value, size_t length) { return storage->writeItem(space, nvs::ItemType::BLOB, key, value, length); }
// IDF 4.4.7 NVSHandleSimple::commit() is a validity check, with no deferred writes.
extern "C" esp_err_t nvs_get_stats(const char*,nvs_stats_t*out){return storage->fillStats(*out);}
extern "C" esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }
#define NUMBER(name, type) extern "C" esp_err_t nvs_get_##name(nvs_handle_t, const char *key, type *out) { return storage->readItem(space, key, *out); }
NUMBER(i8, int8_t) NUMBER(u8, uint8_t) NUMBER(i16, int16_t) NUMBER(u16, uint16_t)
NUMBER(i32, int32_t) NUMBER(u32, uint32_t) NUMBER(i64, int64_t) NUMBER(u64, uint64_t)
#undef NUMBER
#define log_e(...) ((void)0)
enum PreferenceType { PT_I8, PT_U8, PT_I16, PT_U16, PT_I32, PT_U32, PT_I64, PT_U64, PT_STR, PT_BLOB, PT_INVALID };
class Preferences {
  bool _started = true, _readOnly = false;
  uint32_t _handle = 1;
public:
  size_t putString(const char *, const char *);
  size_t putString(const char *, String);
  size_t putBytes(const char *, const void *, size_t);
  PreferenceType getType(const char *);
  bool isKey(const char *);
  String getString(const char *, String);
  size_t getBytesLength(const char *);
  size_t getBytes(const char *, void *, size_t);
};
#include "preferences.inc"
static Preferences prefs;
#ifdef IOTCLOUD_HUB
static const size_t configCapacity = 16384;
#else
static const size_t configCapacity = 6144;
#endif

#include "local_auth.h"
static DynamicJsonDocument config(configCapacity);static bool configRecovery=false,localAuthReady=false;static ph_auth_state localAuth;static ph_auth_store loadedAuthStore;
static char webPassword[33]={};static int authMux;
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
using SemaphoreHandle_t=void*;static SemaphoreHandle_t localAuthMutex;
#define portMAX_DELAY 1
static void*xSemaphoreCreateMutex(){return reinterpret_cast<void*>(1);}static void xSemaphoreTake(void*,int){}static void xSemaphoreGive(void*){}
static uint32_t now=1;static uint32_t millis(){return now;}
static void esp_fill_random(void*p,size_t n){static uint8_t counter;for(size_t i=0;i<n;i++)static_cast<uint8_t*>(p)[i]=++counter;}
static bool hubRestartPending(){return false;}
static void updatePassword(){strlcpy(webPassword,config["auth"]["password"]|"",sizeof(webPassword));}
#include "../../src/auth_storage.inc"
#include "load.inc"
#ifdef DAIKIN_BASELINE
#include "baseline-save.inc"
static bool resetConfiguration(){return false;}
#else
#include "save.inc"
#endif
static void boot(Nor &nor){bootNor(nor);configRecovery=false;localAuthReady=false;config=DynamicJsonDocument(configCapacity);String text=loadAuthConfigText();assert(!deserializeJson(config,text.c_str(),text.length()));config.shrinkToFit();updatePassword();initLocalAuth();assert(localAuthReady&&!configRecovery);}
static void seed(Nor &nor){bootNor(nor);std::string text="{}";assert(prefs.putString("config",text.c_str())==text.size());uint8_t wifi;assert(storage->createOrOpenNamespace("nvs.net80211",true,wifi)==ESP_OK);std::string value(512,'w');assert(storage->writeItem(wifi,nvs::ItemType::BLOB,"wifi-state",value.data(),value.size())==ESP_OK);uint8_t hap;assert(storage->createOrOpenNamespace("HAP",true,hap)==ESP_OK);value.assign(2048,'h');assert(storage->writeItem(hap,nvs::ItemType::BLOB,"pairings",value.data(),value.size())==ESP_OK);}
static void intact(){uint8_t ns;assert(storage->createOrOpenNamespace("HAP",false,ns)==ESP_OK);std::string data(2048,0);assert(storage->readItem(ns,nvs::ItemType::BLOB,"pairings",&data[0],data.size())==ESP_OK);assert(data==std::string(2048,'h'));assert(prefs.getString("config","")=="{}");}
static void fillPairs(){char peer[13],key[33];for(unsigned i=1;i<=12;i++){snprintf(peer,sizeof(peer),"aabbccdd%04x",i);assert(ph_auth_pair(&localAuth,"",peer,7,key));assert(ph_auth_remote_store(&localAuth,peer,key,7,ph_auth_credential_epoch(&localAuth)));}}
static std::string snapshot(){std::string text;serializeJson(config,text);return text;}
static void validBudget() {
  Nor nor; seed(nor); boot(nor);
#ifndef DAIKIN_BASELINE
if(!getenv("DAIKIN_NVS_NO_PAIRS"))fillPairs();
#endif
iotcloud::HubService service(config);
  service.store = saveConfig;
  service.randomHex = [](size_t n) { return std::string(n * 2, 'a'); };
  service.derive = [](const std::string &, const std::string &) { return std::string(64, 'b'); };
  using R = iotcloud::HubService::Request;
  const std::string credentials = R"({"username":"local","password":"local-password"})";
  assert(service.handle(R("POST", "/api/hub/setup", credentials, "", "", true), 0).status == 201);
  auto login = service.handle(R("POST", "/api/auth/login", credentials), 1); assert(login.status == 200);
  for (unsigned i = 0; i < 12; i++) {
    DynamicJsonDocument body(1024); char mac[13]; snprintf(mac, sizeof(mac), "aabbccdd%04x", i + 1);
    body["mac"] = std::string(mac); body["model"] = i == 0 ? "tc1" : i == 1 ? "a1" : "m1"; body["version"] = "v2";
    std::string request; serializeJson(body, request);
    assert(service.handle(R("POST", "/api/devices", request, login.token), i + 2).status == 201);
    assert(config.capacity() < 16384 && iotcloud::HubService::valid(config["hub"]));
    auto expected = snapshot(); boot(nor); assert(loadAuthConfigText() == expected); intact();
  }
  DynamicJsonDocument body(8192); body["enabled"] = true; body["api_key"] = "test-api-key-1234";
  auto maps = body.createNestedArray("mappings");
  for (unsigned i = 0; i < 8; i++) {
    auto map = maps.createNestedObject(); unsigned device = i < 6 ? 0 : 1, channel = i < 6 ? i : i - 6;
    map["device"] = config["hub"]["devices"][device]; map["channel"] = channel;
    map["topic"] = std::string("lab") + std::to_string(i) + (device == 0 ? "001" : channel ? "002" : "003");
  }
  std::string request; serializeJson(body, request);
  auto reply = service.handle(R("PUT", "/api/integrations/bemfa", request, login.token), 20);
  assert(reply.status == 200);
  // Keeping the key must copy it before move-assignment releases the old pool.
  body["api_key"] = ""; request.clear(); serializeJson(body, request);
  assert(service.handle(R("PUT", "/api/integrations/bemfa", request, login.token), 21).status == 200);
  assert(config["hub"]["bemfa"]["api_key"] == "test-api-key-1234");
  body.clear(); body["enabled"] = true; maps = body.createNestedArray("mappings");
  for (unsigned i = 0; i < 8; i++) {
    auto map = maps.createNestedObject(); map["direction"] = "out";
    map["local_topic"] = std::string("lab/local/") + std::to_string(i);
    map["remote_topic"] = std::string("lab/upstream/") + std::to_string(i);
    map["retain"] = false; map["allow_retained"] = false;
  }
  request.clear(); serializeJson(body, request);
  assert(service.handle(R("PUT", "/api/hub/bridge", request, login.token), 22).status == 200);
  auto expected = snapshot(); boot(nor); assert(loadAuthConfigText() == expected); intact();
  printf("matrix pages=%zu codec=%s typical_json=%zu blob=%zu pairs=%u+%u PASS\n",nor.bytes.size()/4096,
#ifdef DAIKIN_BASELINE
"rc3-json",prefs.getBytesLength("config_hub"),prefs.getBytesLength("config_hub"),0u,0u);
#else
"rc4-mp",measureJson(config),prefs.getBytesLength(authConfigKey),localAuth.store.count,localAuth.store.remote_count);
#endif
  for(unsigned round=0;round<12;round++){DynamicJsonDocument edit(configCapacity);edit.set(config);edit["device"]["name"]=std::string("typical-")+std::to_string(round);size_t work=nor.work;assert(saveConfig(edit));assert(nor.work>work);auto previous=snapshot();unsigned in=localAuth.store.count,out=localAuth.store.remote_count;boot(nor);assert(snapshot()==previous&&localAuth.store.count==in&&localAuth.store.remote_count==out);intact();}
  printf("matrix pages=%zu typical 12updates+reboots PASS\n",nor.bytes.size()/4096);
  DynamicJsonDocument maximal(configCapacity);maximal.set(config);maximal["device"]["name"]=std::string(63,'n');
  maximal["hub"]["username"]=std::string(32,'u');for(JsonObject d:maximal["hub"]["devices"].as<JsonArray>())d["username"]=std::string(32,'u');for(JsonObject m:maximal["hub"]["bemfa"]["mappings"].as<JsonArray>())m["device"]["username"]=std::string(32,'u');
  for(JsonObject route:maximal["hub"]["bridge"]["mappings"].as<JsonArray>())for(const char *key:{"local_topic","remote_topic"}){size_t size=measureJson(maximal);if(size<authConfigLimit){std::string topic=route[key].as<std::string>();topic.append(std::min(size_t(512-topic.size()),authConfigLimit-size),'x');route[key]=topic;}}
  assert(measureJson(maximal)==authConfigLimit&&iotcloud::HubService::valid(maximal["hub"]));
  for(unsigned round=0;round<12;round++){DynamicJsonDocument edit(configCapacity);edit.set(maximal);edit["device"]["name"]=std::string(63,char('a'+round));size_t beforeWork=nor.work;bool saved=saveConfig(edit);if(!saved){
#ifndef DAIKIN_BASELINE
assert(nor.work==beforeWork);
#else
(void)beforeWork;
#endif
assert(!configRecovery);auto previous=snapshot();unsigned in=localAuth.store.count,out=localAuth.store.remote_count;boot(nor);assert(snapshot()==previous&&localAuth.store.count==in&&localAuth.store.remote_count==out);intact();printf("matrix pages=%zu legal_max_json=%zu round=%u SAFE_UNCHANGED_REJECTION\n",nor.bytes.size()/4096,measureJson(maximal),round);assert(nor.bytes.size()==16384);return;}boot(nor);intact();}
  printf("matrix pages=%zu legal maximum JSON=%zu blob=%zu 12updates PASS\n",nor.bytes.size()/4096,measureJson(config),prefs.getBytesLength(
#ifdef DAIKIN_BASELINE
"config_hub"
#else
authConfigKey
#endif
));
  printf("12 devices + 8 Bemfa + 8 bridge mappings: JSON=%zu pool=%zu/16384 bytes\n", expected.size(), config.capacity());
}

static void budget(){Nor nor;seed(nor);boot(nor);fillPairs();for(unsigned round=0;round<12;round++){DynamicJsonDocument next(16384);next.set(config);next["_version"]=1;next["padding"]="";size_t base=measureJson(next);next["padding"]=std::string(authConfigLimit-base,char('a'+round));assert(measureJson(next)==authConfigLimit);bool saved=saveConfig(next);if(!saved)fprintf(stderr,"budget round%u recovery%d keysize%zu configlen%zu\n",round,configRecovery,prefs.getBytesLength(authConfigKey),measureJson(next));if(!saved){assert(!configRecovery);boot(nor);assert(localAuth.store.count==12&&localAuth.store.remote_count==12);assert(config["padding"].as<std::string>()==std::string(authConfigLimit-base,char('a'+round-1)));intact();printf("nonbusiness padding boundary safely rejected round%u config%zu\n",round,authConfigLimit);return;}boot(nor);assert(localAuth.store.count==12&&localAuth.store.remote_count==12);assert(measureJson(config)==authConfigLimit);intact();}printf("actual IDF NVS budget %zu config + %zu auth + wifi512 + HAP2048,12updates PASS\n",authConfigLimit,sizeof(ph_auth_store));}
static void power(bool update){Nor before;seed(before);boot(before);char key[33];assert(ph_auth_pair(&localAuth,"","aabbccddee01",2,key));if(update){DynamicJsonDocument next(configCapacity);next.set(config);next["device"]["name"]="before";assert(saveConfig(next));}auto image=before.bytes;DynamicJsonDocument wanted(configCapacity);wanted.set(config);wanted["auth"]["password"]="adminfjr";before.work=0;assert(saveConfig(wanted));size_t bytes=before.work;for(size_t cut=0;cut<=bytes;cut++){Nor nor;nor.bytes=image;boot(nor);DynamicJsonDocument next(configCapacity);next.set(config);next["auth"]["password"]="adminfjr";nor.cut=cut;try{saveConfig(next);}catch(const PowerLoss&){}nor.cut=-1;boot(nor);bool changed=std::string(webPassword)=="adminfjr";assert(changed?localAuth.store.count==0:localAuth.store.count==1);intact();}printf("actual IDF NVS password+pair atomic %s %zu byte cuts PASS\n",update?"update":"first",bytes+1);}
static void failure(){Nor nor;seed(nor);boot(nor);fillPairs();auto old=nor.bytes;nor.fail=true;DynamicJsonDocument next(configCapacity);next.set(config);next["auth"]["password"]="adminfjr";assert(!saveConfig(next));assert(localAuth.store.count==12&&localAuth.store.remote_count==12);nor.fail=false;boot(nor);assert(localAuth.store.count==12&&localAuth.store.remote_count==12&&std::string(webPassword)=="");intact();assert(resetConfiguration());boot(nor);assert(localAuth.store.count==0&&localAuth.store.remote_count==0);}
int main(int argc,char**argv){assert(argc==2);std::string test=argv[1];if(test=="valid-budget")validBudget();else if(test=="budget")budget();else if(test=="power"){power(false);power(true);}else if(test=="failure")failure();else abort();}

// SPDX-License-Identifier: GPL-3.0-or-later
// Production config/Preferences methods and IDF NVS; only Flash I/O is modeled.
#include "nvs_storage.hpp"
#include "hub_service.h"
#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "../host_strings.h"

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
  long failAfter = -1;
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
    if (fail || failAfter == 0) return ESP_ERR_FLASH_OP_FAIL;
    if (offset + size > bytes.size()) return ESP_ERR_INVALID_ARG;
    auto data = static_cast<const uint8_t *>(in);
    for (size_t i = 0; i < size; i++) {
      if (cut == 0) throw PowerLoss();
      if (cut > 0) --cut;
      if (failAfter == 0) return ESP_ERR_FLASH_OP_FAIL;
      if (failAfter > 0) --failAfter;
      assert((uint8_t(~bytes[offset + i]) & data[i]) == 0);
      bytes[offset + i] &= data[i]; ++work;
    }
    return ESP_OK;
  }
  esp_err_t erase_range(size_t offset, size_t size) override {
    if (fail || failAfter == 0) return ESP_ERR_FLASH_OP_FAIL;
    if (offset + size > bytes.size() || offset % 4096 || size % 4096) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < size; i++) {
      if (cut == 0) throw PowerLoss();
      if (cut > 0) --cut;
      if (failAfter == 0) return ESP_ERR_FLASH_OP_FAIL;
      if (failAfter > 0) --failAfter;
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
extern "C" esp_err_t nvs_erase_key(nvs_handle_t, const char *key) { return storage->eraseItem(space, nvs::ItemType::ANY, key); }
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
  bool remove(const char *);
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
static uint8_t randomCounter;
static void esp_fill_random(void*p,size_t n){for(size_t i=0;i<n;i++)static_cast<uint8_t*>(p)[i]=++randomCounter;}
static bool hubRestartPending(){return false;}
static void updatePassword(){strlcpy(webPassword,config["auth"]["password"]|"",sizeof(webPassword));}
#ifdef DAIKIN_FIXED_AUTH_V2
#include "auth_storage_v2_baseline.inc"
#else
#include "../../src/auth_storage.inc"
#endif
#include "load.inc"
static unsigned invalidations=0;static void invalidateBusinessStatusCache(){++invalidations;}
#ifdef DAIKIN_BASELINE
#include "baseline-save.inc"
static bool resetConfiguration(){return false;}
#else
#include "save.inc"
#endif
static void boot(Nor &nor){bootNor(nor);configRecovery=false;localAuthReady=false;config=DynamicJsonDocument(configCapacity);String text=loadAuthConfigText();assert(!deserializeJson(config,text.c_str(),text.length()));config.shrinkToFit();updatePassword();initLocalAuth();assert(localAuthReady&&!configRecovery);}
static void seed(Nor &nor){bootNor(nor);std::string text="{}";assert(prefs.putString("config",text.c_str())==text.size());uint8_t wifi;assert(storage->createOrOpenNamespace("nvs.net80211",true,wifi)==ESP_OK);std::string value(512,'w');assert(storage->writeItem(wifi,nvs::ItemType::BLOB,"wifi-state",value.data(),value.size())==ESP_OK);uint8_t hap;assert(storage->createOrOpenNamespace("HAP",true,hap)==ESP_OK);value.assign(2048,'h');assert(storage->writeItem(hap,nvs::ItemType::BLOB,"pairings",value.data(),value.size())==ESP_OK);}
static void intact(){uint8_t ns;assert(storage->createOrOpenNamespace("HAP",false,ns)==ESP_OK);std::string data(2048,0);assert(storage->readItem(ns,nvs::ItemType::BLOB,"pairings",&data[0],data.size())==ESP_OK);assert(data==std::string(2048,'h'));assert(!prefs.isKey("config")||prefs.getString("config","")=="{}");}
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
static void failure(){Nor nor;seed(nor);boot(nor);fillPairs();auto old=nor.bytes;const auto oldConfig=snapshot();const unsigned beforeInvalidations=invalidations;nor.fail=true;DynamicJsonDocument next(configCapacity);next.set(config);next["auth"]["password"]="adminfjr";assert(!saveConfig(next));assert(snapshot()==oldConfig&&invalidations==beforeInvalidations&&nor.bytes==old);assert(localAuth.store.count==12&&localAuth.store.remote_count==12);nor.fail=false;boot(nor);assert(localAuth.store.count==12&&localAuth.store.remote_count==12&&std::string(webPassword)=="");intact();assert(resetConfiguration());boot(nor);assert(localAuth.store.count==0&&localAuth.store.remote_count==0);}
// A migrated hub BLOB retires its obsolete base config on the next save.
static void dualKey(){
  Nor nor;seed(nor);
  DynamicJsonDocument legacy(8192);legacy["_version"]=1;legacy["auth"]["password"]="adminfjr";legacy["device"]["name"]="synthetic-hub";
  legacy["legacy_padding"]=std::string(1800,'x');std::string text;serializeJson(legacy,text);
  assert(prefs.putBytes("config_hub",text.data(),text.size())==text.size());
  boot(nor);assert(std::string(webPassword)=="adminfjr");
  // The first write replaces the legacy hub BLOB with combined authority.
  DynamicJsonDocument first(configCapacity);first.set(config);bool initial=saveConfig(first);
  assert(initial);assert(authStorageSnapshot().error==AuthStorageError::ok);
  const std::string otherLegacy(1600,'l');assert(prefs.putString("config",otherLegacy.c_str())==otherLegacy.size());
  for(unsigned round=0;round<16;round++){
    auto before=nor.bytes;auto old=snapshot();DynamicJsonDocument next(configCapacity);next.set(config);next["auth"]["password"]=(round%2)?"adminfjr":"";
    bool saved=saveConfig(next);const auto d=authStorageSnapshot();
    assert(d.attempted&&d.stats&&d.required&&d.sizes&&d.oldBytes>0&&d.newBytes>0);
    printf("dual-key pages%zu round%u saved%d free%zu used%zu total%zu required%zu old%zu new%zu error%u\n",nor.bytes.size()/4096,round,saved,d.freeEntries,d.usedEntries,d.totalEntries,d.requiredEntries,d.oldBytes,d.newBytes,unsigned(d.error));
    if(!saved){assert(d.error==AuthStorageError::insufficient_entries);assert(!configRecovery&&nor.bytes==before&&snapshot()==old);}
    assert(prefs.isKey("config_hub")&&!prefs.isKey("config"));
    boot(nor);
  }
}
static std::vector<uint8_t> migrationImage(){
 for(unsigned padding=1;padding<8192;padding+=32){
  Nor nor;seed(nor);DynamicJsonDocument legacy(8192);legacy["_version"]=1;legacy["auth"]["password"]="adminfjr";legacy["device"]["name"]="initial";legacy["padding"]="";
  const size_t target=1758;legacy["padding"]=std::string(target-measureMsgPack(legacy)-2,'p');assert(measureMsgPack(legacy)==target);
  std::string text;serializeJson(legacy,text);assert(prefs.putBytes("config_hub",text.data(),text.size())==text.size());
  std::string other(padding,'c');if(prefs.putBytes("config",other.data(),other.size())!=other.size())continue;
  nvs_stats_t stats={};assert(nvs_get_stats(nullptr,&stats)==ESP_OK);if(stats.free_entries==196)return nor.bytes;
 }
 assert(false);return {};
}
static std::string blobSnapshot(const char *key){std::string value(prefs.getBytesLength(key),0);assert(prefs.getBytes(key,&value[0],value.size())==value.size());return value;}
static void migration(bool faults){
 Nor before;before.bytes=migrationImage();boot(before);assert(!prefs.isKey("authcfg_hub"));const auto image=before.bytes;const auto oldConfig=snapshot();const auto oldLegacy=blobSnapshot("config"),oldHub=blobSnapshot("config_hub");
 DynamicJsonDocument next(configCapacity);next.set(config);next["auth"]["password"]="";before.work=0;bool saved=saveConfig(next);const auto d=authStorageSnapshot();
 assert(d.stats&&d.freeEntries==196&&d.totalEntries==504);
#ifdef DAIKIN_FIXED_AUTH_V2
 assert(d.oldBytes==0);assert(!saved&&d.newBytes==2542&&d.requiredEntries==215&&d.error==AuthStorageError::insufficient_entries&&before.bytes==image&&snapshot()==oldConfig);
 printf("v2 equivalent live layout: free196 required215 new2542 safe reject PASS\n");return;
#else
 assert(d.oldBytes==oldHub.size());assert(saved&&d.newBytes==1766&&d.requiredEntries==191&&d.error==AuthStorageError::ok);boot(before);assert(std::string(webPassword)=="");
 const size_t work=before.work;const auto newConfig=snapshot();const auto newHub=blobSnapshot("config_hub");printf("v3 equivalent live layout: free196 required191 new1766 migrate PASS\n");
 if(!faults)return;
 // Ordinary I/O failure cannot publish a new in-memory credential.
 Nor failed;failed.bytes=image;boot(failed);failed.fail=true;next=DynamicJsonDocument(configCapacity);next.set(config);next["auth"]["password"]="";assert(!saveConfig(next)&&snapshot()==oldConfig&&failed.bytes==image);failed.fail=false;boot(failed);assert(std::string(webPassword)=="adminfjr");
 for(size_t cut=0;cut<=work;cut++){
  Nor nor;nor.bytes=image;boot(nor);next=DynamicJsonDocument(configCapacity);next.set(config);next["auth"]["password"]="";nor.cut=cut;
  try{saveConfig(next);}catch(const PowerLoss&){}nor.cut=-1;boot(nor);
  assert(snapshot()==oldConfig||snapshot()==newConfig);assert(localAuth.store.count==0&&localAuth.store.remote_count==0);
  uint8_t ns;assert(storage->createOrOpenNamespace("nvs.net80211",false,ns)==ESP_OK);std::string wifi(512,0);assert(storage->readItem(ns,nvs::ItemType::BLOB,"wifi-state",&wifi[0],wifi.size())==ESP_OK&&wifi==std::string(512,'w'));
  assert(storage->createOrOpenNamespace("HAP",false,ns)==ESP_OK);std::string hap(2048,0);assert(storage->readItem(ns,nvs::ItemType::BLOB,"pairings",&hap[0],hap.size())==ESP_OK&&hap==std::string(2048,'h'));
  assert(blobSnapshot("config")==oldLegacy);const auto hub=blobSnapshot("config_hub");assert(hub==oldHub||hub==newHub);
 }
 printf("v3 first-authority migration %zu NOR byte cuts preserve WiFi/HAP/legacy PASS\n",work+1);
 Nor repeated;repeated.bytes=image;boot(repeated);next=DynamicJsonDocument(configCapacity);next.set(config);next["auth"]["password"]="";assert(saveConfig(next));
 char key[33];for(unsigned i=0;i<8;i++){
  next=DynamicJsonDocument(configCapacity);next.set(config);next["device"]["name"]=std::string("updated-")+std::to_string(i);assert(saveConfig(next));boot(repeated);assert(config["device"]["name"]==std::string("updated-")+std::to_string(i));
 }
 for(unsigned i=1;i<=3;i++){char peer[13];snprintf(peer,sizeof(peer),"aabbccddee%02x",i);assert(ph_auth_pair(&localAuth,"",peer,PH_AUTH_SCOPE_CONTROL,key));boot(repeated);char token[33];assert(ph_auth_pair_resume(&localAuth,peer,key,token));}assert(!prefs.isKey("authcfg_hub"));
 printf("v3 existing blob atomic migration +8 updates +3 durable pairs PASS\n");
 // Leave budget for another full generation before exercising GC. The live
 // 16 KiB boundary is intentionally allowed to reject growth without I/O;
 // padding is synthetic, not a business field or a reason to relax the guard.
 next=DynamicJsonDocument(configCapacity);next.set(config);
 std::string padding=next["padding"].as<std::string>();assert(padding.size()>128);
 next["padding"]=padding.substr(0,padding.size()-128);assert(saveConfig(next));boot(repeated);
 // Locate a real GC update, then interrupt every programmed/erased byte.
 for(unsigned round=0;round<32;round++){
  const auto gcImage=repeated.bytes;const auto gcOld=snapshot();const size_t initialWork=repeated.work;
  next=DynamicJsonDocument(configCapacity);next.set(config);next["device"]["name"]=std::string("gc-")+std::to_string(round);assert(saveConfig(next));const size_t delta=repeated.work-initialWork;const auto gcNew=snapshot();
  if(delta<4096)continue;
  for(size_t cut=0;cut<=delta;cut++){
   Nor nor;nor.bytes=gcImage;boot(nor);next=DynamicJsonDocument(configCapacity);next.set(config);next["device"]["name"]=std::string("gc-")+std::to_string(round);nor.cut=cut;
   try{saveConfig(next);}catch(const PowerLoss&){}nor.cut=-1;boot(nor);assert(snapshot()==gcOld||snapshot()==gcNew);
   assert(localAuth.store.count==3&&localAuth.store.remote_count==0);assert(!prefs.isKey("config"));
   uint8_t ns;assert(storage->createOrOpenNamespace("nvs.net80211",false,ns)==ESP_OK);std::string wifi(512,0);assert(storage->readItem(ns,nvs::ItemType::BLOB,"wifi-state",&wifi[0],wifi.size())==ESP_OK&&wifi==std::string(512,'w'));
   assert(storage->createOrOpenNamespace("HAP",false,ns)==ESP_OK);std::string hap(2048,0);assert(storage->readItem(ns,nvs::ItemType::BLOB,"pairings",&hap[0],hap.size())==ESP_OK&&hap==std::string(2048,'h'));
  }
  printf("v3 live-equivalent GC update %zu NOR byte cuts preserve config/pair/WiFi/HAP PASS\n",delta+1);return;
 }
 assert(false&&"expected a real GC update");
#endif
}
static void authority(){
 Nor wrong;seed(wrong);assert(prefs.putString("config_hub","{}")>0);assert(loadAuthConfigText()=="invalid");
 Nor good;seed(good);assert(prefs.putBytes("config_hub","{}",2)==2);assert(loadAuthConfigText()=="{}"&&!strcmp(authConfigKey,"config_hub"));
 // A present primary authority cannot be bypassed by legacy credentials.
 assert(prefs.putBytes("authcfg_hub","bad",3)==3);assert(loadAuthConfigText()=="invalid");
 Nor corrupt;seed(corrupt);AuthConfigHeader h{authConfigMagic,99,0,0};assert(prefs.putBytes("config_hub",&h,sizeof(h))==sizeof(h));assert(loadAuthConfigText()=="invalid");
 printf("legacy BLOB selection/STR rejection/authority precedence/corrupt v3 fail-closed PASS\n");
}

#ifndef DAIKIN_FIXED_AUTH_V2
// Synthetic business data only. Match the observed 16 KiB accounting exactly,
// including same-sized updates of six existing routes, without DUT credentials.
static std::vector<uint8_t> liveImage(bool primary,unsigned version=3){
 DynamicJsonDocument value(configCapacity);value["_version"]=1;value["device"]["name"]="Synthetic hub";
 auto hub=value.createNestedObject("hub");hub["schema"]=1;hub["username"]="local";
 hub["salt"]=hub["mqtt_salt"]=std::string(32,'a');hub["password_hash"]=hub["mqtt_hash"]=std::string(64,'b');hub["mqtt_password_separate"]=false;
 auto devices=hub.createNestedArray("devices");
 for(unsigned i=1;i<=2;i++){auto d=devices.createNestedObject();char peer[13];snprintf(peer,sizeof(peer),"aabbccddee%02x",i);d["mac"]=peer;d["model"]=i==1?"tc1":"a1";d["version"]="v2";d["status"]="active";d["username"]="local";}
 hub["bridge"]["enabled"]=true;auto routes=hub["bridge"].createNestedArray("mappings");
 for(unsigned i=0;i<6;i++){auto r=routes.createNestedObject();r["direction"]="out";r["local_topic"]=std::string("lab/local/")+std::to_string(i);r["remote_topic"]=std::string("lab/remote/")+std::to_string(i);r["retain"]=false;r["allow_retained"]=false;}
 ph_auth_store store;ph_auth_store_empty(&store);store.count=store.remote_count=1;
 strcpy(store.pairs[0].peer,"aabbccddee01");memset(store.pairs[0].key,0x6b,16);store.pairs[0].scopes=7;store.remote[0]=store.pairs[0];
 const size_t target=2026-sizeof(AuthConfigHeader)-authStoreWireSize(store);
 for(JsonObject r:routes){
  std::string topic=r["remote_topic"].as<std::string>();
  auto wire=[](size_t n){return n+(n<32?1:n<256?2:3);};
  const size_t remaining=target-measureMsgPack(value);size_t length=512;
  while(wire(length)-wire(topic.size())>remaining)--length;
  topic.append(length-topic.size(),'x');r["remote_topic"]=topic;
 }
 assert(measureMsgPack(value)==target&&iotcloud::HubService::valid(hub));
 std::string legacyHub;serializeJson(value,legacyHub);
 std::string packed;serializeMsgPack(value,packed);const auto &payload=version==1?legacyHub:packed;
 const size_t storedAuth=version==3?authStoreWireSize(store):sizeof(store);std::string blob(sizeof(AuthConfigHeader)+storedAuth+payload.size(),0);AuthConfigHeader h{authConfigMagic,uint16_t(version),0,uint32_t(payload.size())};
 memcpy(&blob[0],&h,sizeof(h));
 if(version==3)authStoreEncode(reinterpret_cast<uint8_t*>(&blob[sizeof(h)]),store);else memcpy(&blob[sizeof(h)],&store,sizeof(store));
 memcpy(&blob[sizeof(h)+storedAuth],payload.data(),payload.size());
 for(unsigned padding=1;padding<6144;padding+=32){
  Nor nor;seed(nor);assert(nor.bytes.size()==16384);
  assert(prefs.putBytes(primary?"authcfg_hub":"config_hub",blob.data(),blob.size())==blob.size());
  if(primary)assert(prefs.putBytes("config_hub",legacyHub.data(),legacyHub.size())==legacyHub.size());
  assert(prefs.putBytes("authcfg_full","other-variant",13)==13);assert(prefs.putBytes("untouched","user-data",9)==9);
  const std::string stale="{\"legacy\":\""+std::string(padding,'p')+"\"}";
  if(prefs.putBytes("config",stale.data(),stale.size())!=stale.size())continue;
  nvs_stats_t stats={};assert(nvs_get_stats(nullptr,&stats)==ESP_OK);
  if(stats.free_entries==198){printf("live fixture key=%s version=%u blob=%zu free=198 used=%zu total=%zu legacy=%zu PASS\n",primary?"authcfg_hub":"config_hub",version,blob.size(),stats.used_entries,stats.total_entries,stale.size());return nor.bytes;}
 }
 assert(false&&"live-equivalent accounting must be reachable");return {};
}
static void liveIntact(){
 uint8_t ns;assert(storage->createOrOpenNamespace("nvs.net80211",false,ns)==ESP_OK);std::string wifi(512,0);assert(storage->readItem(ns,nvs::ItemType::BLOB,"wifi-state",&wifi[0],wifi.size())==ESP_OK&&wifi==std::string(512,'w'));
 assert(storage->createOrOpenNamespace("HAP",false,ns)==ESP_OK);std::string hap(2048,0);assert(storage->readItem(ns,nvs::ItemType::BLOB,"pairings",&hap[0],hap.size())==ESP_OK&&hap==std::string(2048,'h'));
 assert(blobSnapshot("authcfg_full")=="other-variant"&&blobSnapshot("untouched")=="user-data");
}
static void editSixRoutes(DynamicJsonDocument &next,char suffix){
 assert(next.set(config));auto routes=next["hub"]["bridge"]["mappings"].as<JsonArray>();assert(routes.size()==6);
 for(JsonObject route:routes){auto topic=route["remote_topic"].as<std::string>();topic.back()=suffix;route["remote_topic"]=topic;}
 assert(iotcloud::HubService::valid(next["hub"]));
}
enum class LiveUpdate{routes,pair,password};
static bool liveUpdate(LiveUpdate operation,char suffix){
 randomCounter=17;
 if(operation==LiveUpdate::pair){char key[33];return ph_auth_pair(&localAuth,"","aabbccddee02",7,key);}
 DynamicJsonDocument next(configCapacity);editSixRoutes(next,suffix);
 if(operation==LiveUpdate::password)next["auth"]["password"]="new-local";
 return saveConfig(next);
}
static void liveFaults(const std::vector<uint8_t>&image,LiveUpdate operation,char suffix,const char *label){
 Nor before;before.bytes=image;boot(before);const auto old=snapshot();const auto oldStore=localAuth.store;
 before.work=0;assert(liveUpdate(operation,suffix));const size_t work=before.work;const auto wanted=snapshot();const auto newStore=localAuth.store;
 for(bool power:{false,true})for(size_t cut=0;cut<=work;cut++){
  Nor nor;nor.bytes=image;boot(nor);if(power)nor.cut=cut;else nor.failAfter=cut;
  try{
   bool saved=liveUpdate(operation,suffix);
   if(!saved){assert(!power&&configRecovery&&snapshot()==old&&!memcmp(&localAuth.store,&oldStore,sizeof(oldStore)));}
  }catch(const PowerLoss&){assert(power);}
  nor.cut=nor.failAfter=-1;boot(nor);
  bool unchanged=snapshot()==old&&!memcmp(&localAuth.store,&oldStore,sizeof(oldStore));
  bool changed=snapshot()==wanted&&!memcmp(&localAuth.store,&newStore,sizeof(newStore));
  assert(unchanged||changed);liveIntact();
 }
 printf("live %s %zu NOR byte power cuts + %zu ordinary I/O failures, exact old/new config+both pair tables PASS\n",label,work+1,work+1);
 fflush(stdout);
}
static void liveReclaim(bool faults){
 for(bool primary:{false,true}){
  const auto image=liveImage(primary);Nor before;before.bytes=image;boot(before);const auto old=snapshot();
  assert(localAuth.store.count==1&&localAuth.store.remote_count==1&&prefs.getBytesLength(authConfigKey)==2026);
  DynamicJsonDocument next(configCapacity);editSixRoutes(next,'y');assert(measureJson(next)==measureJson(config));
  before.work=0;bool saved=saveConfig(next);const auto d=authStorageSnapshot();const size_t work=before.work;
  printf("live same-size key=%s saved=%d old=%zu new=%zu free=%zu required=%zu error=%u work=%zu\n",authConfigKey,saved,d.oldBytes,d.newBytes,d.freeEntries,d.requiredEntries,unsigned(d.error),work);fflush(stdout);
  assert(saved&&d.oldBytes==2026&&d.newBytes==2026&&d.requiredEntries==199&&d.freeEntries>=d.requiredEntries);
  const auto wanted=snapshot();assert(wanted!=old);assert(!prefs.isKey("config")&&(!primary||!prefs.isKey("config_hub")));liveIntact();boot(before);assert(snapshot()==wanted&&localAuth.store.count==1&&localAuth.store.remote_count==1);
  if(faults){
   const std::string label=primary?"authcfg_hub":"config_hub";
   liveFaults(image,LiveUpdate::routes,'y',(label+" cleanup+same-size").c_str());
   liveFaults(image,LiveUpdate::pair,'y',(label+" cleanup+first-pair").c_str());
   liveFaults(image,LiveUpdate::password,'y',(label+" cleanup+password-revoke").c_str());
   boot(before);bool gc=false;
   for(unsigned round=0;round<20&&!gc;round++){
    const auto gcImage=before.bytes;before.work=0;assert(liveUpdate(LiveUpdate::routes,char('a'+round)));
    gc=before.work>=4096;
    if(gc)liveFaults(gcImage,LiveUpdate::routes,char('a'+round),(label+" actual-GC").c_str());
    boot(before);
   }
   assert(gc&&"must exercise a real GC erase");
  }else{
   for(unsigned round=0;round<20;round++){DynamicJsonDocument edit(configCapacity);editSixRoutes(edit,char('a'+round));assert(saveConfig(edit));const auto expected=snapshot();boot(before);assert(snapshot()==expected&&localAuth.store.count==1&&localAuth.store.remote_count==1);liveIntact();}
   char key[33],token[33];assert(ph_auth_pair(&localAuth,"","aabbccddee02",7,key));boot(before);assert(ph_auth_pair_resume(&localAuth,"aabbccddee02",key,token));
   assert(ph_auth_remote_store(&localAuth,"aabbccddee02",key,7,ph_auth_credential_epoch(&localAuth)));boot(before);assert(localAuth.store.count==2&&localAuth.store.remote_count==2);
   assert(ph_auth_pair_remove(&localAuth,"aabbccddee02"));boot(before);assert(localAuth.store.count==1&&localAuth.store.remote_count==2);
   assert(ph_auth_remote_remove(&localAuth,"aabbccddee02"));boot(before);assert(localAuth.store.count==1&&localAuth.store.remote_count==1);liveIntact();
   // A fresh pairing must itself be able to reclaim stale generations.
   Nor pair;pair.bytes=image;boot(pair);assert(ph_auth_pair(&localAuth,"","aabbccddee02",7,key));boot(pair);assert(ph_auth_pair_resume(&localAuth,"aabbccddee02",key,token));liveIntact();
   printf("live key=%s 20 six-route updates + pair/remote add/remove + first-write pairing durable PASS\n",primary?"authcfg_hub":"config_hub");
  }
 }
}
static void liveLegacy(){
 for(bool primary:{false,true})for(unsigned version:{1u,2u}){
  const auto image=liveImage(primary,version);Nor nor;nor.bytes=image;boot(nor);const auto oldStore=localAuth.store;
  DynamicJsonDocument next(configCapacity);editSixRoutes(next,'y');assert(saveConfig(next));const auto d=authStorageSnapshot();
  assert(d.oldBytes>2026&&d.newBytes==2026&&d.requiredEntries==199&&d.freeEntries>=d.requiredEntries);
  const auto wanted=snapshot();const auto raw=blobSnapshot(authConfigKey);AuthConfigHeader h;memcpy(&h,raw.data(),sizeof(h));assert(h.version==3);
  boot(nor);assert(snapshot()==wanted&&!memcmp(&localAuth.store,&oldStore,sizeof(oldStore)));liveIntact();
  printf("old combined key=%s version=%u old=%zu new=%zu free=%zu required=%zu migration+reboot PASS\n",authConfigKey,version,d.oldBytes,d.newBytes,d.freeEntries,d.requiredEntries);
  const std::string label=std::string(primary?"authcfg_hub":"config_hub")+" v"+std::to_string(version)+" migration";
  liveFaults(image,LiveUpdate::routes,'y',label.c_str());
 }
}
#else
static void liveReclaim(bool){abort();}
static void liveLegacy(){abort();}
#endif
int main(int argc,char**argv){assert(argc==2);std::string test=argv[1];if(test=="authority")authority();else if(test=="live-reclaim")liveReclaim(false);else if(test=="live-reclaim-power")liveReclaim(true);else if(test=="live-legacy")liveLegacy();else if(test=="migration")migration(true);else if(test=="dual-key")dualKey();else if(test=="valid-budget")validBudget();else if(test=="budget")budget();else if(test=="power"){power(false);power(true);}else if(test=="failure")failure();else abort();}

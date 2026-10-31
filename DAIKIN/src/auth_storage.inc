/* One NVS blob is the commit boundary for config and both pairing directions. */
#include <mbedtls/sha256.h>
#include "../include/wire_mp.h"
#include "../include/auth_storage_diagnostics.h"
static AuthStorageDiagnostics authStorageDiagnostics;
static AuthStorageDiagnostics authStorageSnapshot(){portENTER_CRITICAL(&authMux);const auto value=authStorageDiagnostics;portEXIT_CRITICAL(&authMux);return value;}
struct AuthConfigHeader {uint32_t magic;uint16_t version,reserved;uint32_t length;};
static const uint32_t authConfigMagic=0x31415044;
#ifdef IOTCLOUD_HUB
static const char *authConfigDefaultKey="authcfg_hub";
static constexpr size_t authConfigLimit=6144;
#else
static const char *authConfigDefaultKey="authcfg_full";
static constexpr size_t authConfigLimit=3999;
#endif
// Version 3 stores only occupied records, never unused slots or volatile tokens.
static size_t authStoreWireSize(const ph_auth_store &store){return 4+32*(size_t(store.count)+store.remote_count);}
static void authStoreEncode(uint8_t *out,const ph_auth_store &store){
  out[0]=uint8_t(store.count);out[1]=uint8_t(store.count>>8);out[2]=uint8_t(store.remote_count);out[3]=uint8_t(store.remote_count>>8);
  size_t at=4;
  for(unsigned direction=0;direction<2;direction++){
    const auto *records=direction?store.remote:store.pairs;const unsigned count=direction?store.remote_count:store.count;
    for(unsigned i=0;i<count;i++,at+=32){memcpy(out+at,records[i].peer,13);memcpy(out+at+13,records[i].key,16);out[at+29]=records[i].scopes;out[at+30]=out[at+31]=0;}
  }
}
static bool authStoreDecode(const uint8_t *in,size_t n,ph_auth_store &store){
  if(n<4)return false;
  const unsigned count=unsigned(in[0])|(unsigned(in[1])<<8),remote=unsigned(in[2])|(unsigned(in[3])<<8);
  if(count>PH_AUTH_PAIR_SLOTS||remote>PH_AUTH_PAIR_SLOTS||n!=4+32*(count+remote))return false;
  ph_auth_store_empty(&store);store.count=count;store.remote_count=remote;size_t at=4;
  for(unsigned direction=0;direction<2;direction++){
    auto *records=direction?store.remote:store.pairs;const unsigned length=direction?remote:count;
    for(unsigned i=0;i<length;i++,at+=32){memcpy(records[i].peer,in+at,13);memcpy(records[i].key,in+at+13,16);records[i].scopes=in[at+29];records[i].reserved[0]=in[at+30];records[i].reserved[1]=in[at+31];}
  }
  return ph_auth_store_valid(&store);
}
static const char *authConfigKey=authConfigDefaultKey;
#ifdef IOTCLOUD_HUB
// Legacy keys may be retired only while the exact combined generation that was
// fully decoded (or written and read back) remains authoritative.
static bool authConfigAuthorityValid=false;
static size_t authConfigAuthorityBytes=0;
static uint8_t authConfigAuthorityHash[32];
static bool authConfigRememberAuthority(const uint8_t *bytes,size_t size){
  authConfigAuthorityValid=mbedtls_sha256_ret(bytes,size,authConfigAuthorityHash,0)==0;
  authConfigAuthorityBytes=size;return authConfigAuthorityValid;
}
static bool authConfigRetireLegacy(size_t size,const uint8_t hash[32]){
  if(!authConfigAuthorityValid||size!=authConfigAuthorityBytes||memcmp(hash,authConfigAuthorityHash,32))return true;
  for(const char *key:{"config","config_hub"}){
    if(!strcmp(key,authConfigKey))continue;
    // IDF can retain a string and a blob with the same key after a type change.
    // Both are obsolete here; never erase the selected combined authority.
    for(unsigned generation=0;generation<2&&prefs.isKey(key);generation++){
      const auto type=prefs.getType(key);
      if((type!=PT_STR&&type!=PT_BLOB)||!prefs.remove(key))return false;
    }
    if(prefs.isKey(key))return false;
  }
  return true;
}
#endif
static String loadLegacyConfigText();
static String loadAuthConfigText(){
  ph_auth_store_empty(&loadedAuthStore);authConfigKey=authConfigDefaultKey;
#ifdef IOTCLOUD_HUB
  authConfigAuthorityValid=false;
  // Only reuse an existing BLOB key: NVS string->blob can retain both types.
  // Existing combined authority always wins, including corrupt data.
  if(!prefs.isKey(authConfigKey)&&prefs.getType("config_hub")==PT_BLOB)authConfigKey="config_hub";
#endif
  if(!prefs.isKey(authConfigKey))return loadLegacyConfigText();
  const size_t size=prefs.getBytesLength(authConfigKey);
  const bool legacyBlob=strcmp(authConfigKey,authConfigDefaultKey)!=0;
  if(legacyBlob&&size<sizeof(AuthConfigHeader))return loadLegacyConfigText();
  if((!legacyBlob&&size<sizeof(AuthConfigHeader)+4)||size>sizeof(AuthConfigHeader)+sizeof(ph_auth_store)+authConfigLimit)return "invalid";
  std::unique_ptr<uint8_t[]> bytes(new(std::nothrow) uint8_t[size+1]);
  if(!bytes||prefs.getBytes(authConfigKey,bytes.get(),size)!=size)return "invalid";
  AuthConfigHeader header;memcpy(&header,bytes.get(),sizeof(header));
  if(legacyBlob&&header.magic!=authConfigMagic)return loadLegacyConfigText();
  if(header.magic!=authConfigMagic||(header.version!=1&&header.version!=2&&header.version!=3)||header.reserved||header.length>authConfigLimit||header.length>size-sizeof(header))return "invalid";
  const size_t storeLength=size-sizeof(header)-header.length;ph_auth_store store;
  if(header.version==3){if(!authStoreDecode(bytes.get()+sizeof(header),storeLength,store))return "invalid";}
  else {if(storeLength!=sizeof(store))return "invalid";memcpy(&store,bytes.get()+sizeof(header),sizeof(store));if(!ph_auth_store_valid(&store))return "invalid";}
  bytes[size]=0;
  const char *payload=reinterpret_cast<char*>(bytes.get()+sizeof(header)+storeLength);
  DynamicJsonDocument decoded(configCapacity);
  const auto error=header.version==1?deserializeJson(decoded,payload,header.length):deserializeMsgPack(decoded,payload,header.length);
  if(error||!decoded.is<JsonObject>()||decoded.overflowed())return "invalid";
  const size_t jsonLength=measureJson(decoded);if(jsonLength>authConfigLimit)return "invalid";
  String text;if(!text.reserve(jsonLength)||serializeJson(decoded,text)!=jsonLength||text.length()!=jsonLength)return "invalid";
#ifdef IOTCLOUD_HUB
  if(!authConfigRememberAuthority(bytes.get(),size))return "invalid";
#endif
  loadedAuthStore=store;return text;
}
static bool persistAuthConfig(DynamicJsonDocument &candidate,const ph_auth_store *store){
  AuthStorageDiagnostics attempt={};attempt.attempted=true;
  auto finish=[&](AuthStorageError error){attempt.error=error;portENTER_CRITICAL(&authMux);authStorageDiagnostics=attempt;portEXIT_CRITICAL(&authMux);return error==AuthStorageError::ok;};
#ifdef IOTCLOUD_HUB
  if(hubRestartPending())return finish(AuthStorageError::restart_pending);
#endif
  if(configRecovery||candidate.overflowed()||!store||!ph_auth_store_valid(store))return finish(AuthStorageError::invalid_input);
  candidate["_version"]=1;if(candidate.overflowed())return finish(AuthStorageError::invalid_input);
  const size_t length=measureJson(candidate);if(length>authConfigLimit)return finish(AuthStorageError::too_large);
  String text;if(!serializeWireMP(candidate,text)||text.length()>authConfigLimit)return finish(AuthStorageError::encode_failed);
  const size_t storedLength=text.length();
  const size_t storeLength=authStoreWireSize(*store);
  const size_t size=sizeof(AuthConfigHeader)+storeLength+storedLength;
  const size_t oldSize=prefs.isKey(authConfigKey)?prefs.getBytesLength(authConfigKey):0;
  attempt.sizes=true;attempt.oldBytes=oldSize;attempt.newBytes=size;
  if(oldSize>sizeof(AuthConfigHeader)+sizeof(*store)+authConfigLimit){configRecovery=true;return finish(AuthStorageError::old_read_failed);}
  std::unique_ptr<uint8_t[]> bytes(new(std::nothrow) uint8_t[size]);
  std::unique_ptr<uint8_t[]> verify(new(std::nothrow) uint8_t[std::max(oldSize,size)]);
  if(!bytes||!verify)return finish(AuthStorageError::allocation_failed);
  uint8_t oldHash[32];
  if(oldSize){if(prefs.getBytes(authConfigKey,verify.get(),oldSize)!=oldSize){configRecovery=true;return finish(AuthStorageError::old_read_failed);}if(mbedtls_sha256_ret(verify.get(),oldSize,oldHash,0)){configRecovery=true;return finish(AuthStorageError::old_read_failed);}}
  const AuthConfigHeader header{authConfigMagic,3,0,uint32_t(storedLength)};
  memcpy(bytes.get(),&header,sizeof(header));authStoreEncode(bytes.get()+sizeof(header),*store);memcpy(bytes.get()+sizeof(header)+storeLength,text.c_str(),storedLength);
#ifdef IOTCLOUD_HUB
  // The old combined blob still contains all settings and both pairing tables.
  // Retiring only its superseded legacy copies releases live entries before GC;
  // a cut or failure here leaves that combined generation authoritative.
  if(oldSize&&!authConfigRetireLegacy(oldSize,oldHash)){configRecovery=true;return finish(AuthStorageError::write_failed);}
#endif
  // Keep one GC page plus a complete new blob generation before touching flash.
  // Every physical page can contribute at most one new split-blob chunk.
  nvs_stats_t stats={};if(nvs_get_stats(nullptr,&stats)!=ESP_OK)return finish(AuthStorageError::stats_failed);
  attempt.stats=true;attempt.freeEntries=stats.free_entries;attempt.usedEntries=stats.used_entries;attempt.totalEntries=stats.total_entries;
  const size_t pages=stats.total_entries/126;
  const size_t requiredEntries=(size+31)/32+2*pages+1+126;
  attempt.required=true;attempt.requiredEntries=requiredEntries;
  if((oldSize!=size||memcmp(bytes.get(),verify.get(),size))&&stats.free_entries<requiredEntries)return finish(AuthStorageError::insufficient_entries);
  // A failed IDF 4.4.7 multi-page write can leave incomplete chunks in RAM.
  // Do not read that ambiguous generation: recovery blocks auth until reboot.
  if(prefs.putBytes(authConfigKey,bytes.get(),size)!=size){configRecovery=true;return finish(AuthStorageError::write_failed);}
  // A successful acknowledgement still requires exact readback.
  const size_t actual=prefs.isKey(authConfigKey)?prefs.getBytesLength(authConfigKey):0;
  bool read=actual&&actual<=std::max(oldSize,size)&&prefs.getBytes(authConfigKey,verify.get(),actual)==actual;
  if(!read||actual!=size||memcmp(bytes.get(),verify.get(),size)){
    bool unchanged=!actual&&!oldSize;
    if(read&&actual==oldSize){uint8_t hash[32];unchanged=mbedtls_sha256_ret(verify.get(),actual,hash,0)==0&&!memcmp(hash,oldHash,32);}
    if(!unchanged)configRecovery=true;
    return finish(AuthStorageError::verify_failed);
  }
#ifdef IOTCLOUD_HUB
  authConfigRememberAuthority(bytes.get(),size);
#endif
  if(&candidate!=&config){config=std::move(candidate);config.shrinkToFit();}
  updatePassword();return finish(AuthStorageError::ok);
}
#include "lan_route_storage.inc"
static void initLocalAuth(){
  localAuthMutex=xSemaphoreCreateMutex();if(!localAuthMutex){configRecovery=true;return;}
  ph_auth_ops ops={};ops.now=[](void*)->uint32_t{return millis();};ops.random=[](void*,uint8_t *bytes,size_t n){esp_fill_random(bytes,n);return true;};
  ops.lock=[](void*){xSemaphoreTake(localAuthMutex,portMAX_DELAY);};ops.unlock=[](void*){xSemaphoreGive(localAuthMutex);};
  ops.password=[](void*,char out[33]){portENTER_CRITICAL(&authMux);memcpy(out,webPassword,33);portEXIT_CRITICAL(&authMux);return true;};
  ops.save=[](void*,const ph_auth_store *store){return persistLanPairingStore(store);};
  localAuthReady=ph_auth_init(&localAuth,&ops,&loadedAuthStore);if(!localAuthReady)configRecovery=true;
}
extern "C" ph_auth_state *device_local_auth(){return localAuthReady&&!configRecovery?&localAuth:nullptr;}

static inline bool localAuthContextValid(const ph_auth_context *context){return !configRecovery&&localAuthReady&&context&&context->owner==&localAuth&&ph_auth_context_valid(context,PH_AUTH_SCOPE_ADMIN);}

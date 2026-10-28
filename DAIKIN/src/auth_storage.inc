/* One NVS blob is the commit boundary for config and both pairing directions. */
#include <mbedtls/sha256.h>
struct AuthConfigHeader {uint32_t magic;uint16_t version,reserved;uint32_t length;};
static const uint32_t authConfigMagic=0x31415044;
#ifdef IOTCLOUD_HUB
static const char *authConfigKey="authcfg_hub";
static constexpr size_t authConfigLimit=6144;
#else
static const char *authConfigKey="authcfg_full";
static constexpr size_t authConfigLimit=3999;
#endif
static String loadLegacyConfigText();
static String loadAuthConfigText(){
  ph_auth_store_empty(&loadedAuthStore);
  if(!prefs.isKey(authConfigKey))return loadLegacyConfigText();
  const size_t size=prefs.getBytesLength(authConfigKey);
  if(size<sizeof(AuthConfigHeader)+sizeof(ph_auth_store)||size>sizeof(AuthConfigHeader)+sizeof(ph_auth_store)+authConfigLimit)return "invalid";
  std::unique_ptr<uint8_t[]> bytes(new(std::nothrow) uint8_t[size+1]);
  if(!bytes||prefs.getBytes(authConfigKey,bytes.get(),size)!=size)return "invalid";
  AuthConfigHeader header;memcpy(&header,bytes.get(),sizeof(header));
  if(header.magic!=authConfigMagic||header.version!=1||header.reserved||header.length>authConfigLimit||size!=sizeof(header)+sizeof(ph_auth_store)+header.length)return "invalid";
  ph_auth_store store;memcpy(&store,bytes.get()+sizeof(header),sizeof(store));
  if(!ph_auth_store_valid(&store))return "invalid";
  loadedAuthStore=store;bytes[size]=0;
  return String(reinterpret_cast<char*>(bytes.get()+sizeof(header)+sizeof(store)),header.length);
}
static bool persistAuthConfig(DynamicJsonDocument &candidate,const ph_auth_store *store){
#ifdef IOTCLOUD_HUB
  if(hubRestartPending())return false;
#endif
  if(configRecovery||candidate.overflowed()||!store||!ph_auth_store_valid(store))return false;
  candidate["_version"]=1;if(candidate.overflowed())return false;
  const size_t length=measureJson(candidate);if(length>authConfigLimit)return false;
  String text;if(!text.reserve(length)||serializeJson(candidate,text)!=length||text.length()!=length)return false;
  const size_t size=sizeof(AuthConfigHeader)+sizeof(*store)+length;
  const size_t oldSize=prefs.isKey(authConfigKey)?prefs.getBytesLength(authConfigKey):0;
  if(oldSize>sizeof(AuthConfigHeader)+sizeof(*store)+authConfigLimit){configRecovery=true;return false;}
  std::unique_ptr<uint8_t[]> bytes(new(std::nothrow) uint8_t[size]);
  std::unique_ptr<uint8_t[]> verify(new(std::nothrow) uint8_t[std::max(oldSize,size)]);
  if(!bytes||!verify)return false;
  uint8_t oldHash[32];
  if(oldSize){if(prefs.getBytes(authConfigKey,verify.get(),oldSize)!=oldSize){configRecovery=true;return false;}if(mbedtls_sha256_ret(verify.get(),oldSize,oldHash,0)){configRecovery=true;return false;}}
  const AuthConfigHeader header{authConfigMagic,1,0,uint32_t(length)};
  memcpy(bytes.get(),&header,sizeof(header));memcpy(bytes.get()+sizeof(header),store,sizeof(*store));memcpy(bytes.get()+sizeof(header)+sizeof(*store),text.c_str(),length);
  // NVS commit acknowledgement alone is insufficient for an ambiguous failure.
  prefs.putBytes(authConfigKey,bytes.get(),size);
  const size_t actual=prefs.isKey(authConfigKey)?prefs.getBytesLength(authConfigKey):0;
  bool read=actual&&actual<=std::max(oldSize,size)&&prefs.getBytes(authConfigKey,verify.get(),actual)==actual;
  if(!read||actual!=size||memcmp(bytes.get(),verify.get(),size)){
    bool unchanged=!actual&&!oldSize;
    if(read&&actual==oldSize){uint8_t hash[32];unchanged=mbedtls_sha256_ret(verify.get(),actual,hash,0)==0&&!memcmp(hash,oldHash,32);}
    if(!unchanged)configRecovery=true;
    return false;
  }
  if(&candidate!=&config){config=std::move(candidate);config.shrinkToFit();}
  updatePassword();return true;
}
static void initLocalAuth(){
  localAuthMutex=xSemaphoreCreateMutex();if(!localAuthMutex){configRecovery=true;return;}
  ph_auth_ops ops={};ops.now=[](void*)->uint32_t{return millis();};ops.random=[](void*,uint8_t *bytes,size_t n){esp_fill_random(bytes,n);return true;};
  ops.lock=[](void*){xSemaphoreTake(localAuthMutex,portMAX_DELAY);};ops.unlock=[](void*){xSemaphoreGive(localAuthMutex);};
  ops.password=[](void*,char out[33]){portENTER_CRITICAL(&authMux);memcpy(out,webPassword,33);portEXIT_CRITICAL(&authMux);return true;};
  ops.save=[](void*,const ph_auth_store *store){return persistAuthConfig(config,store);};
  localAuthReady=ph_auth_init(&localAuth,&ops,&loadedAuthStore);if(!localAuthReady)configRecovery=true;
}
extern "C" ph_auth_state *device_local_auth(){return localAuthReady&&!configRecovery?&localAuth:nullptr;}

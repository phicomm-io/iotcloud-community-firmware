/* Runs only on the existing UART/command owner. No image-sized ESP allocation. */
static struct {
  uint32_t request=0,session=0,deadline=0,received=0,crc=0,generation=0,at=0;
  uint8_t resource=0,code=0;
  bool pending=false,active=false,have=false;
  const char *state="idle";
} screenWallpaper;
static void screenAppearanceGet(JsonObject out,bool wire) {
  uint8_t p[APPEARANCE_BYTES];
  out["supported"]=bool(screenBridge.capabilities&SCREEN_CAP_APPEARANCE);
  out["revision"]=screenAppearance.revision;
  out["applied_revision"]=screenAppearanceSession==screenBridge.session?screenAppearanceAck:0;
  out["error"]=screenAppearanceError;
  if(wire&&appearance_encode(&screenAppearance,p,sizeof(p)))out["bytes"]=screenBase64(p,sizeof(p));
}
static const char *screenAppearanceSet(JsonObjectConst params,JsonObject out) {
  if(params.size()!=1||!params.containsKey("bytes"))return "invalid_params";
  if(!(screenBridge.capabilities&SCREEN_CAP_APPEARANCE))return "unsupported";
  uint8_t wire[APPEARANCE_BYTES];size_t n;
  std::unique_ptr<screen_appearance> candidate(new(std::nothrow) screen_appearance);
  if(!candidate)return "busy";
  if(!screenUnbase64(params["bytes"],wire,sizeof(wire),n)||
     !appearance_decode(candidate.get(),wire,n)||
     candidate->revision<=screenAppearance.revision)return "invalid_params";
  if(candidate->wallpaper==4&&!(screenBridge.capabilities&SCREEN_CAP_WALLPAPER))return "unsupported";
  if(candidate->language&&!(screenBridge.capabilities&SCREEN_CAP_LANGUAGE))return "unsupported";
  if(candidate->page_rail&&!(screenBridge.capabilities&SCREEN_CAP_PAGE_RAIL))return "unsupported";
  std::swap(screenAppearance,*candidate);
  if(!screenSave()){std::swap(screenAppearance,*candidate);return "storage_error";}
  if(screenAppearance.language!=candidate->language)screenWeatherLanguageChanged();
  screenAppearanceAttempts=0;screenAppearanceSentAt=0;screenAppearanceError="";
  invalidateBusinessStatusCache();screenAppearanceGet(out,true);return "ok";
}
static void screenAppearancePoll() {
  if(!(screenBridge.capabilities&SCREEN_CAP_APPEARANCE)||screenImage.active||screenRecovering)return;
  if(screenAppearanceSession!=screenBridge.session) {
    screenAppearanceSession=screenBridge.session;screenAppearanceAck=0;
    screenAppearanceAttempts=0;screenAppearanceSentAt=0;screenAppearanceError="";
  }
  if(screenAppearanceAck==screenAppearance.revision)return;
  if(screenAppearanceSentAt&&uint32_t(millis()-screenAppearanceSentAt)<3000)return;
  if(screenAppearanceAttempts>=3){screenAppearanceError="ack_timeout";return;}
  uint8_t p[1+APPEARANCE_BYTES]={SCREEN_MSG_APPEARANCE};
  if(screenAppearance.language&&!(screenBridge.capabilities&SCREEN_CAP_LANGUAGE)){screenAppearanceError="language_unsupported";return;}
  if(screenAppearance.page_rail&&!(screenBridge.capabilities&SCREEN_CAP_PAGE_RAIL)){screenAppearanceError="page_rail_unsupported";return;}
  if(appearance_encode(&screenAppearance,p+1,APPEARANCE_BYTES)&&
     bry_bridge_send(&screenBridge,p,sizeof(p),SCREEN_CAP_APPEARANCE)) {
    screenAppearanceSentAt=millis();screenAppearanceAttempts++;
    screenAppearanceError="";
  }
}
static void screenWallpaperReceive(const uint8_t *p,size_t n) {
  if(n!=20||p[1]!=1||p[2]>1||p[3]>WALLPAPER_PREPARING||scene_u32(p+4)!=screenWallpaper.request||
     screenWallpaper.session!=screenBridge.session)return;
  screenWallpaper.pending=false;screenWallpaper.have=true;screenWallpaper.code=p[2];
  screenWallpaper.state=p[2]?"mcu_rejected":"complete";
  screenWallpaper.resource=p[3];screenWallpaper.active=p[3]==6||p[3]==WALLPAPER_PREPARING;
  screenWallpaper.received=scene_u32(p+8);screenWallpaper.crc=scene_u32(p+12);
  screenWallpaper.generation=scene_u32(p+16);screenWallpaper.at=millis();
  invalidateBusinessStatusCache();
}
static bool screenWallpaperSend(unsigned op,uint32_t offset,const uint8_t *bytes,size_t n) {
  uint8_t p[524]={SCREEN_MSG_WALLPAPER,1,(uint8_t)op};
  if(n>512)return false;
  if(!++screenWallpaper.request)++screenWallpaper.request;
  scene_put32(p+4,screenWallpaper.request);scene_put32(p+8,offset);
  if(n)memcpy(p+12,bytes,n);
  if(!bry_bridge_send(&screenBridge,p,n+12,SCREEN_CAP_WALLPAPER))return false;
  screenWallpaper.session=screenBridge.session;screenWallpaper.pending=true;
  screenWallpaper.deadline=millis()+20000;screenWallpaper.state="pending";
  return true;
}
static void screenWallpaperGet(JsonObject out) {
  out["supported"]=bool(screenBridge.capabilities&SCREEN_CAP_WALLPAPER);
  out["available"]=screenWallpaper.have&&screenWallpaper.session==screenBridge.session;
  out["request"]=screenWallpaper.request;out["state"]=screenWallpaper.state;
  out["code"]=screenWallpaper.code;out["resource_state"]=screenWallpaper.resource;
  out["preparing"]=screenWallpaper.resource==WALLPAPER_PREPARING;out["active"]=screenWallpaper.active;out["received"]=screenWallpaper.received;
  out["crc"]=screenWallpaper.crc;out["generation"]=screenWallpaper.generation;
}
static void screenWallpaperPoll(uint32_t now) {
  if(screenWallpaper.session!=screenBridge.session) {
    screenWallpaper.pending=screenWallpaper.active=screenWallpaper.have=false;
    screenWallpaper.state="idle";screenWallpaper.session=screenBridge.session;
  }
  if(screenWallpaper.pending&&(int32_t)(now-screenWallpaper.deadline)>=0) {
    screenWallpaper.pending=false;screenWallpaper.state="timeout";
  }
}
static const char *screenWallpaperCommand(const String &action,JsonObjectConst params,JsonObject out) {
  if(action=="screen.wallpaper.get") {
    if(params.size())return "invalid_params";
    if((screenBridge.capabilities&SCREEN_CAP_WALLPAPER)&&!screenWallpaper.pending&&
       (!screenWallpaper.have||uint32_t(millis()-screenWallpaper.at)>(screenWallpaper.resource==WALLPAPER_PREPARING?500u:screenWallpaper.active?16000u:2000u)))
      screenWallpaperSend(WP_QUERY,0,nullptr,0);
    screenWallpaperGet(out);return "ok";
  }
  if(!(screenBridge.capabilities&SCREEN_CAP_WALLPAPER))return "unsupported";
  if(screenWallpaper.pending||screenImage.active||screenRecovering||screenUploadBusy||screenFontBusy())return "busy";
  uint8_t bytes[512];size_t n=0;unsigned op;uint32_t offset=0;
  if(action=="screen.wallpaper.begin") {
    if(params.size()!=1||!screenUnbase64(params["header"],bytes,32,n)||n!=32||
       memcmp(bytes,"IOTW5651",8)||bytes[12]!=240||bytes[13]||bytes[14]!=171||bytes[15]!=1||
       scene_u32(bytes+20)!=WALLPAPER_BYTES||scene_u32(bytes+24)||
       scene_u32(bytes+28)!=scene_crc32(bytes,28)||screenWallpaper.active)return "invalid_params";
    op=WP_BEGIN;
  } else if(action=="screen.wallpaper.chunk") {
    if(params.size()!=2||!params["offset"].is<uint32_t>()||
       !screenUnbase64(params["bytes"],bytes,sizeof(bytes),n)||!n||
       !screenWallpaper.active)return "invalid_params";
    if(screenWallpaper.resource==WALLPAPER_PREPARING)return "busy";
    offset=params["offset"];if(offset!=screenWallpaper.received||offset>WALLPAPER_BYTES||n>WALLPAPER_BYTES-offset)return "invalid_params";
    op=WP_CHUNK;
  } else if(action=="screen.wallpaper.commit"||action=="screen.wallpaper.cancel") {
    if(params.size())return "invalid_params";
    op=action.endsWith("commit")?WP_COMMIT:WP_CANCEL;
    if(op==WP_COMMIT&&(!screenWallpaper.active||screenWallpaper.received!=WALLPAPER_BYTES))return "invalid_params";
  } else return "unknown_action";
  if(!screenWallpaperSend(op,offset,bytes,n))return "busy";
  screenWallpaperGet(out);return "ok";
}

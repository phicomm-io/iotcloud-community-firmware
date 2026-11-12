/* Optional font banks. Never allocate an image-sized ESP buffer. */
static struct {
  uint32_t request=0,session=0,deadline=0,received=0,total=0,crc=0,generation=0,at=0;
  uint8_t resource=0,code=0;
  bool pending=false,active=false,have=false;
  const char *state="idle";
} screenFont;
static bool screenFontBusy(){return screenFont.active||screenFont.pending;}
static void screenFontReceive(const uint8_t *p,size_t n) {
  if(n!=20||p[1]!=1||p[2]>1||p[3]>FONT_PREPARING||scene_u32(p+4)!=screenFont.request||
     screenFont.session!=screenBridge.session)return;
  screenFont.pending=false;screenFont.have=true;screenFont.code=p[2];
  screenFont.state=p[2]?"mcu_rejected":"complete";
  screenFont.resource=p[3];screenFont.active=p[3]==6||p[3]==FONT_PREPARING;
  screenFont.received=scene_u32(p+8);screenFont.crc=scene_u32(p+12);
  screenFont.generation=scene_u32(p+16);screenFont.at=millis();
  invalidateBusinessStatusCache();
}
static bool screenFontSend(unsigned op,uint32_t offset,const uint8_t *bytes,size_t n) {
  uint8_t p[524]={SCREEN_MSG_FONT,1,(uint8_t)op};
  if(n>512)return false;
  if(!++screenFont.request)++screenFont.request;
  scene_put32(p+4,screenFont.request);scene_put32(p+8,offset);
  if(n)memcpy(p+12,bytes,n);
  if(!bry_bridge_send(&screenBridge,p,n+12,SCREEN_CAP_FONT_BANK))return false;
  screenFont.session=screenBridge.session;screenFont.pending=true;
  screenFont.deadline=millis()+20000;screenFont.state="pending";
  return true;
}
static void screenFontGet(JsonObject out) {
  out["supported"]=bool(screenBridge.capabilities&SCREEN_CAP_FONT_BANK);
  out["available"]=screenFont.have&&screenFont.session==screenBridge.session;
  out["request"]=screenFont.request;out["state"]=screenFont.state;
  out["code"]=screenFont.code;out["resource_state"]=screenFont.resource;
  out["preparing"]=screenFont.resource==FONT_PREPARING;out["active"]=screenFont.active;out["received"]=screenFont.received;
  out["total"]=screenFont.total;out["crc"]=screenFont.crc;out["generation"]=screenFont.generation;
}
static void screenFontPoll(uint32_t now) {
  if(screenFont.session!=screenBridge.session) {
    screenFont.pending=screenFont.active=screenFont.have=false;
    screenFont.state="idle";screenFont.session=screenBridge.session;
  }
  if(screenFont.pending&&(int32_t)(now-screenFont.deadline)>=0) {
    screenFont.pending=false;screenFont.state="timeout";
  }
}
static const char *screenFontCommand(const String &action,JsonObjectConst params,JsonObject out) {
  if(action=="screen.font.get") {
    if(params.size())return "invalid_params";
    if((screenBridge.capabilities&SCREEN_CAP_FONT_BANK)&&!screenFont.pending&&
       (!screenFont.have||uint32_t(millis()-screenFont.at)>(screenFont.resource==FONT_PREPARING?500u:screenFont.active?16000u:2000u)))
      screenFontSend(WP_QUERY,0,nullptr,0);
    screenFontGet(out);return "ok";
  }
  if(!(screenBridge.capabilities&SCREEN_CAP_FONT_BANK))return "unsupported";
  if(screenFont.pending||screenImage.active||screenRecovering||screenUploadBusy||screenWallpaper.active||screenWallpaper.pending)return "busy";
  uint8_t bytes[512];size_t n=0;unsigned op;uint32_t offset=0;
  if(action=="screen.font.begin") {
    if(params.size()!=1||!screenUnbase64(params["header"],bytes,32,n)||n!=32||
       memcmp(bytes,"IOTFNT02",8)||(scene_u32(bytes+8)<32||scene_u32(bytes+8)>SCREEN_RESOURCE_LIMIT||scene_u32(bytes+28)!=scene_crc32(bytes,28))||screenFont.active)return "invalid_params";
    screenFont.total=scene_u32(bytes+8);
    op=WP_BEGIN;
  } else if(action=="screen.font.chunk") {
    if(params.size()!=2||!params["offset"].is<uint32_t>()||
       !screenUnbase64(params["bytes"],bytes,sizeof(bytes),n)||!n||
       !screenFont.active)return "invalid_params";
    if(screenFont.resource==FONT_PREPARING)return "busy";
    offset=params["offset"];if(offset!=screenFont.received||offset>screenFont.total||n>screenFont.total-offset)return "invalid_params";
    op=WP_CHUNK;
  } else if(action=="screen.font.commit"||action=="screen.font.cancel") {
    if(params.size())return "invalid_params";
    op=action.endsWith("commit")?WP_COMMIT:WP_CANCEL;
    if(op==WP_COMMIT&&(!screenFont.active||screenFont.received!=screenFont.total))return "invalid_params";
  } else return "unknown_action";
  if(!screenFontSend(op,offset,bytes,n))return "busy";
  screenFontGet(out);return "ok";
}

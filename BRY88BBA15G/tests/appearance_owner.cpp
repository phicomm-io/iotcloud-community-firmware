#include <ArduinoJson.h>
#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
extern "C" {
#include "protocol/appearance.h"
#include "protocol/scene.h"
#include "protocol/wallpaper.h"
#include "bridge.h"
}
struct String:std::string {using std::string::string;bool endsWith(const char *s)const{return size()>=strlen(s)&&!compare(size()-strlen(s),strlen(s),s);}};
static bry_bridge screenBridge;
static screen_appearance screenAppearance;
static uint32_t screenAppearanceAck,screenAppearanceSession,screenAppearanceSentAt;
static unsigned screenAppearanceAttempts;
static const char *screenAppearanceError="";
static bool screenRecovering,screenUploadBusy,saveOk=true,sendOk=true;
static struct {bool active=false;}screenImage;
static uint32_t ms;
static unsigned saves;
static std::vector<uint8_t> sent;
static uint32_t millis(){return ms;}
static bool screenSave(){saves++;return saveOk;}
static void invalidateBusinessStatusCache(){}
static bool brySend(void *,const char *p,size_t n){if(sendOk)sent.assign(p,p+n);return sendOk;}
/* Test transport strings use hex; the real APP1/UART encoder is not stubbed. */
static std::string screenBase64(const uint8_t *p,size_t n){const char *h="0123456789abcdef";std::string s;for(size_t i=0;i<n;i++){s+=h[p[i]>>4];s+=h[p[i]&15];}return s;}
static bool screenUnbase64(JsonVariantConst v,uint8_t *p,size_t cap,size_t &n){
 if(!v.is<const char*>())return false;
 const char *s=v.as<const char*>();n=strlen(s)/2;if(strlen(s)%2||n>cap)return false;
 for(size_t i=0;i<n;i++){unsigned b;if(sscanf(s+i*2,"%2x",&b)!=1)return false;p[i]=b;}return true;
}
static unsigned languageChanges;
static void screenWeatherLanguageChanged(){languageChanges++;}
static bool screenFontBusy();
#include "screen_appearance.inc"
#include "screen_font.inc"
static std::vector<uint8_t> payload(){uint8_t p[SCREEN_LINK_CHUNK];size_t n=sizeof(p);uint32_t session,seq;assert(screen_link_decode((const char*)sent.data(),sent.size()-2,&session,&seq,p,&n));return {p,p+n};}
int main(){
 screenBridge.send=brySend;screenBridge.extended=true;screenBridge.session=17;screenBridge.capabilities=SCREEN_CAP_APPEARANCE|SCREEN_CAP_WALLPAPER;
 appearance_default(&screenAppearance);ms=1;screenAppearancePoll();auto p=payload();assert(p.size()==705&&p[0]==SCREEN_MSG_APPEARANCE);
 for(unsigned i=0;i<3;i++){ms+=3000;screenAppearancePoll();}assert(screenAppearanceAttempts==3&&!strcmp(screenAppearanceError,"ack_timeout"));
 screenAppearanceAck=screenAppearance.revision;screenAppearanceError="";auto previous=sent;ms+=3000;screenAppearancePoll();assert(sent==previous);
 screenBridge.session++;screenAppearancePoll();assert(screenAppearanceAttempts==1&&screenAppearanceAck==0);
 DynamicJsonDocument params(4096),out(4096);uint8_t wire[APPEARANCE_BYTES];auto candidate=screenAppearance;candidate.revision++;candidate.theme=5;assert(appearance_encode(&candidate,wire,sizeof(wire)));params["bytes"]=screenBase64(wire,sizeof(wire));
 saveOk=false;assert(!strcmp(screenAppearanceSet(params.as<JsonObjectConst>(),out.to<JsonObject>()),"storage_error"));assert(screenAppearance.theme==0&&screenAppearance.revision==1);
 saveOk=true;assert(!strcmp(screenAppearanceSet(params.as<JsonObjectConst>(),out.to<JsonObject>()),"ok"));assert(screenAppearance.theme==5&&screenAppearance.revision==2&&screenAppearanceAttempts==0);
 assert(!strcmp(screenAppearanceSet(params.as<JsonObjectConst>(),out.to<JsonObject>()),"invalid_params"));
 screenBridge.capabilities=0;ms+=3000;previous=sent;screenAppearancePoll();assert(sent==previous);assert(!strcmp(screenAppearanceSet(params.as<JsonObjectConst>(),out.to<JsonObject>()),"unsupported"));
 // A pending resource command cannot be overwritten, and stale ACKs are ignored.
 screenBridge.capabilities=SCREEN_CAP_APPEARANCE|SCREEN_CAP_WALLPAPER;screenWallpaperPoll(ms);
 uint8_t header[32]={};memcpy(header,"IOTW5651",8);header[12]=240;header[14]=171;header[15]=1;scene_put32(header+16,0x12345678);scene_put32(header+20,WALLPAPER_BYTES);scene_put32(header+28,scene_crc32(header,28));
 params.clear();params["header"]=screenBase64(header,32);auto call=[&](const char *a){out.clear();return screenWallpaperCommand(String(a),params.as<JsonObjectConst>(),out.to<JsonObject>());};
 screenImage.active=true;assert(!strcmp(call("screen.wallpaper.begin"),"busy"));screenImage.active=false;
 screenUploadBusy=true;assert(!strcmp(call("screen.wallpaper.begin"),"busy"));screenUploadBusy=false;
 screenRecovering=true;assert(!strcmp(call("screen.wallpaper.begin"),"busy"));screenRecovering=false;
 assert(!strcmp(call("screen.wallpaper.begin"),"ok"));p=payload();assert(p[2]==WP_BEGIN&&p.size()==44&&screenWallpaper.pending);
 assert(!strcmp(call("screen.wallpaper.begin"),"busy"));
 uint8_t ack[20]={SCREEN_MSG_WALLPAPER_STATUS,1,0,6};scene_put32(ack+4,screenWallpaper.request-1);screenWallpaperReceive(ack,20);assert(screenWallpaper.pending);
 scene_put32(ack+4,screenWallpaper.request);screenWallpaperReceive(ack,20);assert(screenWallpaper.active&&!screenWallpaper.pending);
 ack[3]=WALLPAPER_PREPARING;screenWallpaperReceive(ack,20);assert(screenWallpaper.active);
 params.clear();params["offset"]=0;params["bytes"]=std::string("aabb");assert(!strcmp(call("screen.wallpaper.chunk"),"busy"));
 params.clear();ms+=501;assert(!strcmp(call("screen.wallpaper.get"),"ok"));assert(payload()[2]==WP_QUERY);
 ack[3]=6;scene_put32(ack+4,screenWallpaper.request);screenWallpaperReceive(ack,20);
 params.clear();params["offset"]=1;params["bytes"]=std::string("aabb");assert(!strcmp(call("screen.wallpaper.chunk"),"invalid_params"));
 params["offset"]=0;assert(!strcmp(call("screen.wallpaper.chunk"),"ok"));p=payload();assert(p[2]==WP_CHUNK&&p.size()==14&&scene_u32(p.data()+8)==0);
 scene_put32(ack+4,screenWallpaper.request);scene_put32(ack+8,2);screenWallpaperReceive(ack,20);params.clear();assert(!strcmp(call("screen.wallpaper.commit"),"invalid_params"));
 // An abandoned upload expires at the MCU; a fresh query allows another upload.
 ms+=16001;assert(!strcmp(call("screen.wallpaper.get"),"ok"));p=payload();assert(p[2]==WP_QUERY);
 ack[3]=RESOURCE_TIMEOUT;scene_put32(ack+4,screenWallpaper.request);screenWallpaperReceive(ack,20);assert(!screenWallpaper.active);
 params["header"]=screenBase64(header,32);assert(!strcmp(call("screen.wallpaper.begin"),"ok"));ms+=20001;screenWallpaperPoll(ms);assert(!screenWallpaper.pending&&!strcmp(screenWallpaper.state,"timeout"));
 screenBridge.session++;screenWallpaperPoll(ms);assert(!screenWallpaper.have&&!screenWallpaper.active);
 // Language requires peer support and only refreshes weather after persistence.
 candidate=screenAppearance;candidate.revision++;candidate.language=1;
 assert(appearance_encode(&candidate,wire,sizeof(wire)));params.clear();params["bytes"]=screenBase64(wire,sizeof(wire));
 assert(!strcmp(screenAppearanceSet(params.as<JsonObjectConst>(),out.to<JsonObject>()),"unsupported"));
 screenBridge.capabilities|=SCREEN_CAP_LANGUAGE;saveOk=false;
 assert(!strcmp(screenAppearanceSet(params.as<JsonObjectConst>(),out.to<JsonObject>()),"storage_error")&&!languageChanges);
 saveOk=true;assert(!strcmp(screenAppearanceSet(params.as<JsonObjectConst>(),out.to<JsonObject>()),"ok")&&languageChanges==1);
 // Rail sizes require negotiation; failed persistence cannot alter the live
 // document. Replay to an older MCU is withheld until its capability returns.
 candidate=screenAppearance;candidate.revision++;candidate.page_rail=20;
 assert(appearance_encode(&candidate,wire,sizeof(wire)));params.clear();params["bytes"]=screenBase64(wire,sizeof(wire));
 auto savedCount=saves;previous=sent;
 assert(!strcmp(screenAppearanceSet(params.as<JsonObjectConst>(),out.to<JsonObject>()),"unsupported")&&saves==savedCount);
 screenBridge.capabilities|=SCREEN_CAP_PAGE_RAIL;saveOk=false;
 assert(!strcmp(screenAppearanceSet(params.as<JsonObjectConst>(),out.to<JsonObject>()),"storage_error")&&!screenAppearance.page_rail);
 saveOk=true;assert(!strcmp(screenAppearanceSet(params.as<JsonObjectConst>(),out.to<JsonObject>()),"ok")&&screenAppearance.page_rail==20);
 screenBridge.capabilities&=~SCREEN_CAP_PAGE_RAIL;ms+=3000;screenAppearancePoll();
 assert(sent==previous&&!strcmp(screenAppearanceError,"page_rail_unsupported"));
 screenBridge.capabilities|=SCREEN_CAP_PAGE_RAIL;ms+=3000;screenAppearancePoll();
 p=payload();assert(p.size()==705&&p[58]==20&&!strcmp(screenAppearanceError,""));
 // Actual font owner: interlocks, preparation queries, stale ACKs, sequential chunks and commit.
 screenBridge.capabilities|=SCREEN_CAP_FONT_BANK;screenFontPoll(ms);
 uint8_t font[57]={};memcpy(font,"IOTFNT02",8);scene_put32(font+8,sizeof(font));
 font[16]=1;font[18]=24;font[20]=36;font[21]=2;font[22]=2;font[23]=32;
 scene_put32(font+28,scene_crc32(font,28));
 params.clear();params["header"]=screenBase64(font,32);
 auto fontCall=[&](const char *action){out.clear();return screenFontCommand(String(action),params.as<JsonObjectConst>(),out.to<JsonObject>());};
 screenWallpaper.active=true;assert(!strcmp(fontCall("screen.font.begin"),"busy"));screenWallpaper.active=false;
 screenImage.active=true;assert(!strcmp(fontCall("screen.font.begin"),"busy"));screenImage.active=false;
 assert(!strcmp(fontCall("screen.font.begin"),"ok"));assert(screenFont.pending&&payload()[0]==SCREEN_MSG_FONT);
 uint8_t fa[20]={SCREEN_MSG_FONT_STATUS,1,0,FONT_PREPARING};scene_put32(fa+4,screenFont.request-1);
 screenFontReceive(fa,20);assert(screenFont.pending);scene_put32(fa+4,screenFont.request);screenFontReceive(fa,20);
 assert(screenFont.active&&!screenFont.pending);params.clear();params["offset"]=0;params["bytes"]=screenBase64(font,sizeof(font));
 assert(!strcmp(fontCall("screen.font.chunk"),"busy"));
 params.clear();ms+=501;assert(!strcmp(fontCall("screen.font.get"),"ok")&&payload()[2]==WP_QUERY);
 fa[3]=RESOURCE_UPLOADING;scene_put32(fa+4,screenFont.request);screenFontReceive(fa,20);
 params.clear();assert(!strcmp(fontCall("screen.font.commit"),"invalid_params"));
 params["offset"]=1;params["bytes"]=screenBase64(font,sizeof(font));assert(!strcmp(fontCall("screen.font.chunk"),"invalid_params"));
 params["offset"]=0;assert(!strcmp(fontCall("screen.font.chunk"),"ok")&&payload().size()==69);
 scene_put32(fa+4,screenFont.request);scene_put32(fa+8,sizeof(font));screenFontReceive(fa,20);
 params.clear();assert(!strcmp(fontCall("screen.font.commit"),"ok"));
 fa[3]=RESOURCE_READY;scene_put32(fa+4,screenFont.request);scene_put32(fa+12,123);scene_put32(fa+16,1);screenFontReceive(fa,20);
 assert(!screenFontBusy()&&screenFont.crc==123&&screenFont.generation==1);
 screenBridge.session++;screenFontReceive(fa,20);screenFontPoll(ms);assert(!screenFont.have&&!screenFont.active);

}

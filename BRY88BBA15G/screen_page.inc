#include "protocol/page_control.h"
static struct {
  uint32_t session=0,at=0,request=0,deadline=0,revision=0;
  unsigned current=0,mask=0,count=0;
  bool received=false,asleep=false;
  const char *state="idle";
} screenPage;
static void screenPageReceive(const uint8_t *p,size_t n){
  if(n!=SCREEN_PAGE_STATUS_BYTES||p[1]!=1||p[2]>2||p[3]>1||p[11]||
      !p[8]||!p[10]||p[10]>8||p[8]>p[10]||!p[9]||p[9]>>p[10])return;
  if(screenPage.session!=screenBridge.session){screenPage={};screenPage.session=screenBridge.session;}
  screenPage.at=millis();screenPage.received=true;screenPage.current=p[8];
  screenPage.asleep=p[3];screenPage.mask=p[9];screenPage.count=p[10];screenPage.revision=scene_u32(p+12);
  if(scene_u32(p+4)&&scene_u32(p+4)==screenPage.request)
    screenPage.state=p[2]?(p[2]==1?"invalid_page":"mcu_busy"):"complete";
}
static void screenPageStatus(JsonObject o){
  bool available=screenPage.received&&screenPage.session==screenBridge.session&&
      uint32_t(millis()-screenPage.at)<5000;
  o["supported"]=(screenBridge.capabilities&SCREEN_CAP_PAGE_CONTROL)!=0;
  o["available"]=available;o["request"]=screenPage.request;
  o["state"]=!strcmp(screenPage.state,"pending")&&int32_t(millis()-screenPage.deadline)>=0?"timeout":screenPage.state;
  if(!available)return;
  o["current"]=screenPage.current;o["enabled_mask"]=screenPage.mask;o["count"]=screenPage.count;
  o["asleep"]=screenPage.asleep;o["layout_revision"]=screenPage.revision;o["age_ms"]=millis()-screenPage.at;
}
static const char *screenPageCommand(const String &action,JsonObjectConst params,JsonObject data){
  if(action=="screen.page.get"){
    if(params.size())return "invalid_params";
    screenPageStatus(data);return "ok";
  }
  if(!(screenBridge.capabilities&SCREEN_CAP_PAGE_CONTROL))return "unsupported";
  if(params.size()!=1||!screenFields(params,"|page||direction|"))return "invalid_params";
  uint8_t p[SCREEN_PAGE_REQUEST_BYTES]={SCREEN_MSG_PAGE_REQUEST,1};
  if(params.containsKey("page")){
    if(!params["page"].is<unsigned>())return "invalid_params";
    unsigned page=params["page"];
    if(!page||page>screenScene.pages||!screenScene.page[page-1].enabled)return "invalid_params";
    p[2]=1;p[8]=page;
  }else {
    const char *direction=params["direction"]|"";
    if(!strcmp(direction,"next"))p[2]=2;
    else if(!strcmp(direction,"previous"))p[2]=3;
    else return "invalid_params";
  }
  if(screenImage.active||screenUploadBusy||screenRecovering||!screenGuardAvailable()||screenGuard.mode||
      (!strcmp(screenPage.state,"pending")&&int32_t(screenPage.deadline-millis())>0))return "busy";
  uint32_t id=screenPage.request+1;if(!id)id=1;scene_put32(p+4,id);
  if(!bry_bridge_send(&screenBridge,p,sizeof(p),SCREEN_CAP_PAGE_CONTROL))return "busy";
  screenPage.session=screenBridge.session;screenPage.request=id;
  screenPage.deadline=millis()+5000;screenPage.state="pending";
  data["request"]=id;return "accepted";
}

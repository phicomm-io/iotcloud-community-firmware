/* One UART reply page in DRAM; the bounded full trace stays on the MCU.
 * Reading diagnostics does not allocate a second frame or stop normal work. */
static uint8_t screenDiagReply[SCREEN_DIAG_REPLY_MAX];
static size_t screenDiagBytes=0;
static uint32_t screenDiagRequest=0,screenDiagSession=0,screenDiagAt=0,screenDiagDeadline=0;
static bool screenDiagPending=false;
static void screenDiagReceive(const uint8_t *p,size_t n){
  if(!screenDiagPending||screenDiagSession!=screenBridge.session||
     n<SCREEN_DIAG_SUMMARY_BYTES||n>sizeof(screenDiagReply)||p[1]!=1||p[2]>2||
     (p[3]&~7)||scene_u32(p+4)!=screenDiagRequest)return;
  uint32_t offset=scene_u32(p+52),count=scene_u32(p+56),stored=scene_u32(p+44);
  if(stored>SCREEN_DIAG_TRACE||offset>SCREEN_DIAG_TRACE||count>SCREEN_DIAG_BATCH||
     n!=SCREEN_DIAG_SUMMARY_BYTES+count*SCREEN_DIAG_RECORD_BYTES||
     (count&&offset+count>stored))return;
  for(unsigned i=0;i<count;i++){
    const uint8_t *r=p+SCREEN_DIAG_SUMMARY_BYTES+SCREEN_DIAG_RECORD_BYTES*i;
    if(r[0]<1||r[0]>2||r[1]>1||(unsigned)(r[2]|r[3]<<8)!=offset+i)return;
  }
  memcpy(screenDiagReply,p,n);screenDiagBytes=n;
  screenDiagPending=false;screenDiagAt=millis();
}
static const char *screenDiagCommand(const String &action,JsonObjectConst params,JsonObject data){
  if(action=="screen.diagnostics.get"){
    if(params.size())return "invalid_params";
    data["supported"]=(screenBridge.capabilities&SCREEN_CAP_DISPLAY_DIAG)!=0;
    data["request"]=screenDiagRequest;
    data["pending"]=screenDiagPending&&int32_t(screenDiagDeadline-millis())>0;
    data["timeout"]=screenDiagPending&&int32_t(screenDiagDeadline-millis())<=0;
    data["available"]=screenDiagBytes&&screenDiagSession==screenBridge.session;
    if(!data["available"].as<bool>())return "ok";
    const uint8_t *p=screenDiagReply;
    data["reply_request"]=scene_u32(p+4);data["age_ms"]=millis()-screenDiagAt;
    data["status"]=p[2];data["active"]=(p[3]&1)!=0;data["waiting"]=(p[3]&2)!=0;
    data["enabled"]=(p[3]&4)!=0;
    const char *keys[]={"generation","page","elapsed_us","flush_count","flush_total_us",
      "flush_max_us","monitor_count","monitor_total_ms","monitor_pixels","stored","dropped",
      "offset","returned","draw_rows","frame_count","frame_period_us","frame_min_us",
      "frame_max_us","framebuffer","draw_buffer","draw_pixels","lcd_cr","core_hz",
      "lcd_divider","horizontal_total","vertical_total","timer_hz","scan_source",
      "render_flags","dma_flushes"};
    for(unsigned i=0;i<sizeof(keys)/sizeof(*keys);i++)data[keys[i]]=scene_u32(p+8+4*i);
    JsonArray records=data.createNestedArray("records");
    for(unsigned i=0;i<scene_u32(p+56);i++){
      const uint8_t *r=p+SCREEN_DIAG_SUMMARY_BYTES+SCREEN_DIAG_RECORD_BYTES*i;
      JsonObject o=records.createNestedObject();o["index"]=r[2]|r[3]<<8;o["kind"]=r[0];o["last"]=r[1]!=0;
      JsonArray area=o.createNestedArray("area");
      for(unsigned k=0;k<4;k++)area.add((int16_t)(r[4+2*k]|r[5+2*k]<<8));
      o["at_us"]=scene_u32(r+12);o["duration_us"]=scene_u32(r+16);o["pixels"]=scene_u32(r+20);
    }
    return "ok";
  }
  if(!(screenBridge.capabilities&SCREEN_CAP_DISPLAY_DIAG))return "unsupported";
  if(screenImage.active||screenUploadBusy||
      (screenDiagPending&&int32_t(screenDiagDeadline-millis())>0))return "busy";
  uint8_t p[SCREEN_DIAG_REQUEST_BYTES]={SCREEN_MSG_DIAG_REQUEST,1};
  if(action=="screen.diagnostics.start"){
    if(!screenFields(params,"|mode||page|"))return "invalid_params";
    if(params.containsKey("mode")&&!params["mode"].is<const char *>())return "invalid_params";
    const char *mode=params["mode"]|"current";p[2]=1;
    if(!strcmp(mode,"next"))p[3]=1;
    else if(!strcmp(mode,"page")){
      if(!params["page"].is<unsigned>()||params["page"].as<unsigned>()<1||
          params["page"].as<unsigned>()>8)return "invalid_params";
      p[3]=2;p[11]=params["page"];
    }else if(strcmp(mode,"current"))return "invalid_params";
    if(p[3]!=2&&params.containsKey("page"))return "invalid_params";
  }else if(action=="screen.diagnostics.read"){
    if(!screenFields(params,"|offset||limit|")||
       (params.containsKey("offset")&&!params["offset"].is<unsigned>())||
       (params.containsKey("limit")&&!params["limit"].is<unsigned>()))return "invalid_params";
    unsigned offset=params["offset"]|0u,limit=params["limit"]|0u;
    if(offset>SCREEN_DIAG_TRACE||limit>SCREEN_DIAG_BATCH)return "invalid_params";
    p[2]=2;p[8]=offset;p[9]=offset>>8;p[10]=limit;
  }else if(action=="screen.diagnostics.stop"){
    if(params.size())return "invalid_params";p[2]=3;
  }else return "unsupported";
  uint32_t id=screenDiagRequest+1;if(!id)id=1;scene_put32(p+4,id);
  if(!bry_bridge_send(&screenBridge,p,sizeof(p),SCREEN_CAP_DISPLAY_DIAG))return "busy";
  if(screenDiagSession!=screenBridge.session)screenDiagBytes=0;
  screenDiagRequest=id;screenDiagPending=true;
  screenDiagSession=screenBridge.session;screenDiagDeadline=millis()+5000;
  data["request"]=id;return "accepted";
}

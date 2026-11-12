/* One owner, persistent policy and negotiated single-packet replacement. */
static void screenPolicyGet(JsonObject out) {
  const display_policy &p = screenDisplayPolicy;
  out["supported"] = bool(screenBridge.capabilities & SCREEN_CAP_DISPLAY);
  out["revision"] = p.revision; out["applied_revision"] = screenPolicyAck;
  out["error"] = screenPolicyError; out["max_plans"] = DISPLAY_PLANS;
  out["ceiling"] = p.ceiling; out["external_minutes"] = p.external_minutes;
  out["battery_minutes"] = p.battery_minutes;
  out["idle_brightness"] = p.idle_brightness; out["idle_page"] = p.idle_page;
  JsonArray plans = out.createNestedArray("plans");
  for (unsigned i = 0; i < p.count; i++) {
    const display_plan &r = p.plans[i]; JsonObject o = plans.createNestedObject();
    o["name"] = r.name; o["enabled"] = r.enabled; o["days"] = r.days;
    o["start"] = r.start; o["end"] = r.end; o["brightness"] = r.brightness; o["page"] = r.page;
  }
}
static bool screenPolicyPage(unsigned page) {
  return page == DISPLAY_KEEP_PAGE ||
         (page < screenScene.pages && screenScene.page[page].enabled);
}
static const char *screenPolicySet(JsonObjectConst params, JsonObject out) {
  if (!screenBridge.extended || !(screenBridge.capabilities & SCREEN_CAP_DISPLAY))
    return "unsupported";
  if (screenImage.active) return "busy";
  if (params.size() != 6 || !params["ceiling"].is<unsigned>() ||
      !params["external_minutes"].is<unsigned>() || !params["battery_minutes"].is<unsigned>() ||
      !params["idle_brightness"].is<unsigned>() || !params["idle_page"].is<unsigned>() ||
      !params["plans"].is<JsonArrayConst>()) return "invalid_params";
  for (JsonPairConst field : params) {
    const char *k=field.key().c_str();
    if (strcmp(k,"ceiling")&&strcmp(k,"external_minutes")&&strcmp(k,"battery_minutes")&&
        strcmp(k,"idle_brightness")&&strcmp(k,"idle_page")&&strcmp(k,"plans"))return "invalid_params";
  }
  unsigned ceiling=params["ceiling"],external=params["external_minutes"],battery=params["battery_minutes"],
           idle=params["idle_brightness"],page=params["idle_page"];
  if (!ceiling || ceiling>100 || external>1440 || battery>1440 || idle>100 || !screenPolicyPage(page))
    return "invalid_params";
  display_policy next = {};
  next.revision = screenDisplayPolicy.revision + 1;
  if (!next.revision) return "invalid_params";
  next.ceiling=ceiling;next.external_minutes=external;next.battery_minutes=battery;
  next.idle_brightness=idle;next.idle_page=page;
  for (JsonObjectConst o : params["plans"].as<JsonArrayConst>()) {
    if (next.count >= DISPLAY_PLANS || o.size()!=7 || !o["name"].is<const char *>() ||
        !o["enabled"].is<bool>() || !o["days"].is<unsigned>() || !o["start"].is<unsigned>() ||
        !o["end"].is<unsigned>() || !o["brightness"].is<unsigned>() || !o["page"].is<unsigned>() ||
        !scene_utf8(o["name"],32)) return "invalid_params";
    unsigned days=o["days"],start=o["start"],end=o["end"],brightness=o["brightness"],dest=o["page"];
    if (days>127 || start>=1440 || end>=1440 || brightness>100 || !screenPolicyPage(dest))return "invalid_params";
    display_plan &r=next.plans[next.count++];r.enabled=o["enabled"];r.days=days;
    r.start=start;r.end=end;r.brightness=brightness;r.page=dest;
    bry::textCopy(r.name,sizeof(r.name),o["name"]);
  }
  if (!display_policy_valid(&next)) return "invalid_params";
  display_policy previous=screenDisplayPolicy;screenDisplayPolicy=next;
  if (!screenSave()) {screenDisplayPolicy=previous;return "storage_error";}
  screenPolicyAck=screenPolicySentAt=0;screenPolicyError="";
  screenPolicyGet(out);invalidateBusinessStatusCache();return "accepted";
}
static void screenPolicyPoll() {
  if (!screenBridge.extended || !(screenBridge.capabilities & SCREEN_CAP_DISPLAY) || screenImage.active)return;
  if (screenPolicySession!=screenBridge.session) {
    screenPolicySession=screenBridge.session;screenPolicyAck=screenPolicySentAt=0;screenPolicyError="";
  }
  if (screenPolicyAck==screenDisplayPolicy.revision)return;
  if (screenPolicySentAt && uint32_t(millis()-screenPolicySentAt)<3000)return;
  uint8_t wire[DISPLAY_POLICY_BYTES];
  if (display_policy_encode(&screenDisplayPolicy,wire,sizeof(wire)) &&
      bry_bridge_send(&screenBridge,wire,sizeof(wire),SCREEN_CAP_DISPLAY))screenPolicySentAt=millis();
}
static void screenPolicyRemap(const screen_scene &oldScene,const screen_scene &next) {
  auto remap=[&](uint8_t &page) {
    if (page==DISPLAY_KEEP_PAGE)return;
    uint32_t id=page<oldScene.pages?oldScene.page[page].id:0;
    for(unsigned i=0;i<next.pages;i++)if(next.page[i].enabled&&id&&next.page[i].id==id) {page=i;return;}
    page=DISPLAY_KEEP_PAGE;
  };
  display_policy old=screenDisplayPolicy;
  remap(screenDisplayPolicy.idle_page);
  for(unsigned i=0;i<screenDisplayPolicy.count;i++)remap(screenDisplayPolicy.plans[i].page);
  if(memcmp(&old,&screenDisplayPolicy,sizeof(old))) {
    if(++screenDisplayPolicy.revision==0)screenDisplayPolicy.revision=1;
    screenPolicyAck=screenPolicySentAt=0;
  }
}

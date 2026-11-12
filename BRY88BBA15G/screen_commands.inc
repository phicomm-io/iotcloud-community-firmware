static bool screenFields(JsonObjectConst o, const char *fields) {
  for (JsonPairConst p : o) {
    String key = "|" + String(p.key().c_str()) + "|";
    if (String(fields).indexOf(key) < 0)
      return false;
  }
  return true;
}
static bool screenText(JsonVariantConst v, size_t cap) {
  return v.is<const char *>() && scene_utf8(v.as<const char *>(), cap);
}
static void screenRuleJson(JsonObject o, const reminder_rule &r) {
  o["id"] = r.id;
  o["kind"] = r.kind;
  o["epoch"] = r.epoch;
  o["minute"] = r.minute;
  o["days"] = r.days;
  o["wake"] = r.wake;
  o["enabled"] = r.enabled;
  o["title"] = r.title;
  o["text"] = r.text;
  o["last_fire"] = r.last_fire;
}
// Keep legacy command locals off the media call path. UDP dispatch already
// holds its receive/response buffers on the Arduino loop task's stack.
static __attribute__((noinline)) const char *screenLegacyServicesCommand(
    const String &action, JsonObjectConst params, JsonObject data) {
  if(action.startsWith("screen.calibration."))return screenCalibrationCommand(action,params,data);
  if(action=="screen.page.get"||action=="screen.page.set")return screenPageCommand(action,params,data);
  if(action.startsWith("screen.diagnostics."))return screenDiagCommand(action,params,data);
  if(action=="screen.appearance.get") {
    if(params.size())return "invalid_params";
    screenAppearanceGet(data,true);return "ok";
  }
  if(action=="screen.appearance.set")return screenAppearanceSet(params,data);
  if(action.startsWith("screen.font."))return screenFontCommand(action,params,data);
  if(action.startsWith("screen.wallpaper."))return screenWallpaperCommand(action,params,data);
  if (action == "screen.module.get") {
    if (params.size()) return "invalid_params";
    screenModuleStatus(data); return "ok";
  }
  if (action == "screen.module.set") return screenModuleSet(params, data);
  if (action == "screen.resources.get") {
    if(params.size())return "invalid_params";
    screenResourceStatus(data);return "ok";
  }
  if (action == "screen.display.get") {
    if (params.size()) return "invalid_params";
    screenPolicyGet(data); return "ok";
  }
  if (action == "screen.display.set") return screenPolicySet(params, data);
  if(action=="screen.startup.get") {
    if(params.size())return "invalid_params";
    screenStartupGet(data);return "ok";
  }
  if(action=="screen.startup.set")return screenStartupSet(params,data);
  if (action == "screen.recovery.get") {
    if (params.size())
      return "invalid_params";
    screenGuardStatus(data, false);
    return "ok";
  }
  if (action == "screen.recovery.nor.set") {
    if(params.size()!=1||!params["mode"].is<const char *>())return "invalid_params";
    const char *mode=params["mode"];unsigned bus;
    if(!strcmp(mode,"sfc_quad"))bus=GUARD_NOR_SFC_QUAD;
    else if(!strcmp(mode,"sfc_single"))bus=GUARD_NOR_SFC_SINGLE;
    else if(!strcmp(mode,"gpio"))bus=GUARD_NOR_GPIO;
    else return "invalid_params";
    if(screenGuard.mode==0)return "not_ready";
    const char *result=screenGuardRequest(GUARD_NOR_MODE,bus,0,data);
    if(!strcmp(result,"accepted"))screenStartupManual();
    return result;
  }
  if (action == "screen.recovery.hold" || action == "screen.recovery.resume" ||
      action == "screen.recovery.enter") {
    if (params.size())
      return "invalid_params";
    const char *result=screenGuardRequest(action.endsWith("hold") ? GUARD_HOLD :
                              action.endsWith("resume") ? GUARD_RESUME : GUARD_RESET,
                              0, 0, data);
    if(!strcmp(result,"accepted"))screenStartupManual();
    return result;
  }
  if (action == "screen.boot.read") {
    if (params.size() != 2 || !params["offset"].is<uint32_t>() ||
        !params["length"].is<unsigned>())
      return "invalid_params";
    uint32_t at = params["offset"];
    unsigned n = params["length"];
    if (at >= SCREEN_BOOT_BYTES || !n || n > SCREEN_BOOT_CHUNK ||
        n > SCREEN_BOOT_BYTES - at)
      return "invalid_params";
    return screenGuardRequest(GUARD_READ_BOOT, at, n, data);
  }
  if (action == "screen.boot.result.get") {
    if (params.size() != 1 || !params["request"].is<uint32_t>())
      return "invalid_params";
    if (!params["request"].as<uint32_t>() ||
        params["request"].as<uint32_t>() != screenGuard.request)
      return "not_found";
    screenGuardStatus(data, true);
    return "ok";
  }
  if (action == "screen.layout.get") {
    if (!screenFields(params, "|offset||limit|"))
      return "invalid_params";
    unsigned at = params["offset"] | 0u, n = params["limit"] | 512u;
    unsigned total = scene_wire_size(&screenScene);
    if (at >= total || !n || n > 512)
      return "invalid_params";
    if (!screenSceneWire.prepare(screenScene)) return "busy";
    n = min(n, total - at);
    uint8_t bytes[512];
    if (!screenSceneWire.read(at, bytes, n)) return "busy";
    data["revision"] = screenScene.revision;
    data["total"] = total;
    data["crc"] = screenSceneWire.crc();
    data["offset"] = at;
    data["bytes"] = screenBase64(bytes, n);
    return "ok";
  }
  if (action == "screen.layout.begin") {
    if (params.size() != 3 || !screenFields(params, "|revision||total||crc|") ||
        !params["revision"].is<uint32_t>() || !params["total"].is<unsigned>() ||
        !params["crc"].is<uint32_t>() ||
        params["revision"].as<uint32_t>() <= screenScene.revision)
      return "invalid_params";
    if(screenWeatherBusy)return "busy";
    uint8_t p[13] = {SC_MSG_BEGIN};
    scene_put32(p + 1, params["revision"]);
    scene_put32(p + 5, params["total"]);
    scene_put32(p + 9, params["crc"]);
    return screenLayoutUpload.receive(&screenScene, p, sizeof(p), millis())
               ? "ok"
               : "invalid_params";
  }
  if (action == "screen.layout.chunk") {
    if (params.size() != 2 || !screenFields(params, "|offset||bytes|") ||
        !params["offset"].is<uint32_t>())
      return "invalid_params";
    uint8_t p[517] = {SC_MSG_CHUNK};
    scene_put32(p + 1, params["offset"]);
    size_t n;
    if (!screenUnbase64(params["bytes"], p + 5, 512, n) || !n)
      return "invalid_params";
    return screenLayoutUpload.receive(&screenScene, p, n + 5, millis())
               ? "ok"
               : "invalid_params";
  }
  if (action == "screen.layout.commit") {
    if (params.size() != 1 || !params["revision"].is<uint32_t>())
      return "invalid_params";
    std::unique_ptr<screen_scene> previous(new (std::nothrow)
                                               screen_scene(screenScene));
    if (!previous)
      return "busy";
    uint8_t p[5] = {SC_MSG_COMMIT};
    scene_put32(p + 1, params["revision"]);
    if (!screenLayoutUpload.receive(&screenScene, p, sizeof(p), millis()))
      return "invalid_params";
    if (!(screenBridge.capabilities & SCREEN_CAP_COMPACT_WIDGETS))
      for(unsigned i=0;i<screenScene.count;i++)if(scene_requires_compact(&screenScene.item[i])) {
        screenScene=*previous;return "unsupported";
      }
    if (!(screenBridge.capabilities & SCREEN_CAP_IMAGE))
      for(unsigned i=0;i<screenScene.count;i++)if(screenScene.item[i].type==SC_IMAGE) {
        screenScene=*previous;return "unsupported";
      }
    display_policy previousPolicy = screenDisplayPolicy;
    screenPolicyRemap(*previous, screenScene);
    if (!screenSave()) {
      screenScene = *previous; screenDisplayPolicy = previousPolicy;
      return "storage_error";
    }
    screenSyncStep = 0;
    screenSyncDeadline = 0;
    screenSyncRetryAt = 0;
    screenSceneAck = 0;
    data["revision"] = screenScene.revision;
    return "accepted";
  }
  if (action == "screen.data.series") {
    if (!(screenBridge.capabilities & SCREEN_CAP_SCENE_V3)) return "unsupported";
    if (params.size()!=3 || !params["id"].is<unsigned>() || !params["ttl"].is<unsigned>() ||
        !params["series"].is<JsonArrayConst>()) return "invalid_params";
    unsigned id=params["id"],ttl=params["ttl"];
    const scene_item *item=nullptr;
    for(unsigned i=0;i<screenScene.count;i++)if(screenScene.item[i].id==id)item=&screenScene.item[i];
    if(!item || item->binding!=SC_BIND_EXTERNAL || !ttl || ttl>86400)return "invalid_params";
    JsonArrayConst all=params["series"];
    if(!all.size() || all.size()>SCENE_SERIES)return "invalid_params";
    unsigned count=all[0]["values"].size();
    if(!count || count>SCENE_SERIES_POINTS)return "invalid_params";
    unsigned maskBytes=(count+7)/8,stride=48+maskBytes+count*4;
    uint8_t wire[SCREEN_LINK_CHUNK]={SC_MSG_SERIES,1,(uint8_t)id,(uint8_t)(id>>8),(uint8_t)all.size(),(uint8_t)count};
    scene_put32(wire+6,ttl);unsigned k=0;
    const char *unit=nullptr;
    bool additive=item->chart==SC_CH_STACK_BAR || item->chart==SC_CH_STACK_AREA || item->chart==SC_CH_RING;
    for(JsonObjectConst o:all) {
      if(o.size()!=3 || !o["name"].is<const char *>() || !o["unit"].is<const char *>() ||
         !scene_utf8(o["name"],32) || !scene_utf8(o["unit"],16) ||
         !o["values"].is<JsonArrayConst>() || o["values"].size()!=count)return "invalid_params";
      const char *u=o["unit"];if(additive&&unit&&strcmp(unit,u))return "invalid_params";unit=u;
      uint8_t *q=wire+12+k++*stride;strcpy((char *)q,o["name"]);strcpy((char *)q+32,u);
      unsigned i=0;
      for(JsonVariantConst v:o["values"].as<JsonArrayConst>()) {
        float f=0;
        if(!v.isNull()) {
          if(!v.is<float>())return "invalid_params";
          f=v.as<float>();if(!isfinite(f)||fabsf(f)>1e9f||(additive&&f<0))return "invalid_params";
          q[48+i/8]|=1u<<(i%8);
        }
        uint32_t bits;memcpy(&bits,&f,4);scene_put32(q+48+maskBytes+i++*4,bits);
      }
    }
    return bry_bridge_send(&screenBridge,wire,12+all.size()*stride,SCREEN_CAP_SCENE_V3)?"accepted":"not_ready";
  }
  if (action == "screen.data.push") {
    if (!screenFields(params, "|id||values||text||ttl|") ||
        !params["id"].is<unsigned>() || !params["ttl"].is<unsigned>())
      return "invalid_params";
    unsigned id = params["id"], ttl = params["ttl"];
    bool found = false;
    for (auto &it : screenScene.item)
      if (it.id == id && it.binding == SC_BIND_EXTERNAL)
        found = true;
    JsonArrayConst values = params["values"];
    const char *text = params["text"] | "";
    size_t len = strlen(text);
    if (!found || !id || id > 65535 || ttl < 1 || ttl > 86400 ||
        !scene_utf8(text, 193) ||
        params.containsKey("values") &&
            !params["values"].is<JsonArrayConst>() ||
        values.size() > 64)
      return "invalid_params";
    /* Exact data ABI: id[1..2], count[3], ttl[4..7], text_length[8]. */
    uint8_t wire[9 + 192 + 64 * 4] = {SC_MSG_DATA};
    wire[1] = id;
    wire[2] = id >> 8;
    wire[3] = values.size();
    scene_put32(wire + 4, ttl);
    wire[8] = len;
    memcpy(wire + 9, text, len);
    unsigned index = 0;
    for (JsonVariantConst v : values) {
      if (!v.is<float>())
        return "invalid_params";
      float f = v.as<float>();
      if (!isfinite(f) || fabsf(f) > 1e9)
        return "invalid_params";
      uint32_t bits;
      memcpy(&bits, &f, 4);
      scene_put32(wire + 9 + len + 4 * index++, bits);
    }
    scene_data check = {};
    size_t size = 9 + len + 4 * values.size();
    if (!scene_data_receive(&check, wire, size, millis()))
      return "invalid_params";
    return bry_bridge_send(&screenBridge, wire, size, SCREEN_CAP_SCENE)
               ? "ok"
               : "not_ready";
  }
  if (action == "screen.reminders.get") {
    if (!screenFields(params, "|offset|"))
      return "invalid_params";
    unsigned at = params["offset"] | 0u;
    if (at >= REMINDER_MAX)
      return "invalid_params";
    data["timezone"] = screenSchedule.timezone;
    data["wake"] = screenSchedule.wake;
    data["offset"] = at;
    JsonArray rules = data.createNestedArray("rules"),
              notices = data.createNestedArray("notices");
    for (unsigned i = at; i < min(at + 8, (unsigned)REMINDER_MAX); i++) {
      if (screenSchedule.rules[i].id)
        screenRuleJson(rules.createNestedObject(), screenSchedule.rules[i]);
      auto &r = screenNotices[i];
      if (r.id) {
        JsonObject o = notices.createNestedObject();
        o["id"] = r.id;
        o["at"] = r.at;
        o["unread"] = r.unread;
        o["title"] = r.title;
        o["text"] = r.text;
      }
    }
    return "ok";
  }
  if (action == "screen.reminders.configure") {
    if (params.size() != 2 || !params["timezone"].is<int>() ||
        !params["wake"].is<unsigned>())
      return "invalid_params";
    int tz = params["timezone"];
    unsigned wake = params["wake"];
    if (tz < -720 || tz > 840 || wake > 2)
      return "invalid_params";
    int old = screenSchedule.timezone;
    uint8_t oldWake = screenSchedule.wake;
    screenSchedule.timezone = tz;
    screenSchedule.wake = wake;
    if (!screenSave()) {
      screenSchedule.timezone = old;
      screenSchedule.wake = oldWake;
      return "storage_error";
    }
    screenSchedule.started = false;
    screenWeatherGeneration++;
    return "ok";
  }
  if (action == "screen.reminders.set") {
    if (!screenFields(
            params,
            "|id||kind||epoch||minute||days||wake||enabled||title||text|") ||
        !params["id"].is<uint32_t>() || !params["kind"].is<unsigned>() ||
        !params["enabled"].is<bool>() || !screenText(params["title"], 65) ||
        !screenText(params["text"], 128))
      return "invalid_params";
    unsigned kind = params["kind"], wake = params["wake"] | 255u,
             minute = params["minute"] | 0u, days = params["days"] | 0u;
    if (kind > 2 || wake > 2 && wake != 255 || minute >= 1440 || days > 127)
      return "invalid_params";
    reminder_rule r = {};
    r.id = params["id"];
    r.kind = kind;
    r.epoch = params["epoch"] | 0u;
    r.minute = minute;
    r.days = days;
    r.wake = wake;
    r.enabled = params["enabled"];
    bry::textCopy(r.title, sizeof(r.title), params["title"]);
    bry::textCopy(r.text, sizeof(r.text), params["text"]);
    if (r.id >= 0x80000000u || !reminder_validate(&r))
      return "invalid_params";
    unsigned slot = REMINDER_MAX;
    for (unsigned i = 0; i < REMINDER_MAX; i++) {
      if (screenSchedule.rules[i].id == r.id) {
        slot = i;
        break;
      }
      if (slot == REMINDER_MAX && !screenSchedule.rules[i].id)
        slot = i;
    }
    if (slot == REMINDER_MAX)
      return "busy";
    auto old = screenSchedule.rules[slot];
    r.last_fire = old.id == r.id ? old.last_fire : 0;
    screenSchedule.rules[slot] = r;
    if (!screenSave()) {
      screenSchedule.rules[slot] = old;
      return "storage_error";
    }
    return "ok";
  }
  if (action == "screen.reminders.delete") {
    if (params.size() != 1 || !params["id"].is<uint32_t>())
      return "invalid_params";
    uint32_t id = params["id"];
    for (auto &r : screenSchedule.rules)
      if (id && r.id == id) {
        auto old = r;
        r = {};
        if (!screenSave()) {
          r = old;
          return "storage_error";
        }
        return "ok";
      }
    return "not_found";
  }
  if (action == "screen.reminders.push") {
    if (!screenFields(params, "|id||title||text||wake|") ||
        !screenText(params["title"], 65) || !screenText(params["text"], 128) ||
        !params["wake"].is<unsigned>() || params["wake"].as<unsigned>() > 2)
      return "invalid_params";
    if (params.containsKey("id") && !params["id"].is<uint32_t>())
      return "invalid_params";
    uint32_t id =
        params.containsKey("id") ? params["id"].as<uint32_t>() : screenNoticeId;
    if (id < 0x80000000u)
      return "invalid_params";
    screenNoticeId = id == UINT32_MAX ? 0x80000000u : id + 1;
    if (!screenNotice(id, params["title"], params["text"], params["wake"]))
      return "storage_error";
    data["id"] = id;
    return "accepted";
  }
  if (action == "screen.reminders.ack") {
    if (params.size() != 1 || !params["id"].is<uint32_t>())
      return "invalid_params";
    uint32_t id = params["id"];
    for (auto &r : screenNotices)
      if (r.id == id && id) {
        bool old = r.unread;
        r.unread = false;
        if (!screenSave()) {
          r.unread = old;
          return "storage_error";
        }
        reminder_notice ack = r;
        ack.wake = REM_UNREAD_ONLY;
        screenNoticeWire(ack);
        return "ok";
      }
    return "not_found";
  }
  if (action == "screen.weather.get") {
    if (params.size())
      return "invalid_params";
    data["config"] = screenWeatherConfig;
    data["config"].remove("key");
    data["config"]["key_set"] = strlen(screenWeatherConfig["key"] | "") > 0;
    data["config"]["verify_tls"] = screenWeatherConfig["verify_tls"] | false;
    data["error"] = screenWeatherError;
    data["request_stage"] = screenWeatherStage;
    data["http_status"] = screenWeatherHttpStatus;
    data["tls_error"] = screenWeatherTlsError;
    data["tls_verify_flags"] = screenWeatherVerifyFlags;
    data["connect_ms"] = screenWeatherConnectMs;
    data["connect_errno"] = screenWeatherConnectErrno;
    data["remote_ip"] = screenWeatherRemoteIp;
    data["busy"] = screenWeatherBusy;
    data["retry_in_ms"] = screenWeatherRetry.remaining(millis());
    data["consecutive_failures"] = screenWeatherRetry.failures;
    data["attempts"] = screenWeatherAttempts;
    data["request_free_byte_heap"] = screenWeatherStartHeap;
    data["result_free_byte_heap"] = screenWeatherEndHeap;
    data["request_largest_byte_block"] = screenWeatherStartBlock;
    data["free_byte_heap"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    data["largest_byte_block"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if (screenWeatherStackFree)
      data["worker_stack_free_bytes"] = screenWeatherStackFree;
    else
      data["worker_stack_free_bytes"] = nullptr;
    data["valid"] = screenWeather.valid && !screenWeather.expired;
    data["provider"] = screenWeather.provider;
    data["location"] = screenWeather.location;
    data["text"] = screenWeather.text;
    data["observed"] = screenWeather.observed;
    if (screenWeather.fields & 1)
      data["temperature"] = screenWeather.temperature;
    if (screenWeather.fields & 2)
      data["feels_like"] = screenWeather.feels;
    if (screenWeather.fields & 4)
      data["humidity"] = screenWeather.humidity;
    if (screenWeather.fields & 8)
      data["wind_speed"] = screenWeather.wind;
    JsonArray days = data.createNestedArray("days");
    for (const auto &day : screenWeather.days)
      if (day.valid) {
        JsonObject d = days.createNestedObject();
        d["date"] = day.date;
        d["text"] = day.text;
        d["low"] = day.low;
        d["high"] = day.high;
      }
    data["delivery"] = screenBridge.extended ? "community" : "factory";
    if (!screenBridge.extended && screenWeatherStockSentAt) {
      data["factory"] = screenWeatherStockLast;
      data["factory_sent_age_ms"] = millis() - screenWeatherStockSentAt;
    }
    if (!screenBridge.extended && screenWeatherStockSentAt && screenWeatherStockSentValid)
      data["last_sent_age_ms"] = millis() - screenWeatherStockSentAt;
    else if (screenBridge.extended && screenWeatherSentSession == screenBridge.session && screenWeatherSentAt)
      data["last_sent_age_ms"] = millis() - screenWeatherSentAt;
    else
      data["last_sent_age_ms"] = nullptr;
    return "ok";
  }
  if (action == "screen.weather.set") {
    if (!screenFields(params, "|enabled||provider||host||key||clear_key||verify_tls||"
                              "latitude||longitude||label||interval|") ||
        !params["enabled"].is<bool>() ||
        (params.containsKey("verify_tls") && !params["verify_tls"].is<bool>()))
      return "invalid_params";
    DynamicJsonDocument next(screenWeatherConfigBytes);
    next.set(screenWeatherConfig);
    for (JsonPairConst kv : params)
      if (strcmp(kv.key().c_str(), "clear_key"))
        next[kv.key()] = kv.value();
    if (params["clear_key"] | false)
      next.remove("key");
    const char *provider = next["provider"] | "", *key = next["key"] | "",
               *host = next["host"] | "";
    unsigned interval = next["interval"] | 900u;
    next["interval"] = interval;
    if (strcmp(provider, "qweather") && strcmp(provider, "seniverse") &&
        strcmp(provider, "openweather") && strcmp(provider, "openmeteo"))
      return "invalid_params";
    if (next.overflowed() || strlen(host)>127 || strlen(key) > 256 || !screenText(next["label"], 49) || interval < 300 ||
        interval > 86400 || !next["latitude"].is<float>() ||
        !next["longitude"].is<float>())
      return "invalid_params";
    float lat = next["latitude"], lon = next["longitude"];
    if (!isfinite(lat) || !isfinite(lon) || lat < -90 || lat > 90 ||
        lon < -180 || lon > 180)
      return "invalid_params";
    for (const char *p = key; *p; p++)
      if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_')
        return "invalid_params";
    if ((next["enabled"] | false) && strcmp(provider, "openmeteo") && !*key)
      return "invalid_params";
    if (!strcmp(provider, "qweather")) {
      String h = host;
      if (h.length() < 16 || h.length() > 127 ||
          !h.endsWith(".qweatherapi.com"))
        return "invalid_params";
      for (char c : h)
        if (!isalnum((unsigned char)c) && c != '-' && c != '.')
          return "invalid_params";
    }
    if (!strcmp(provider, "openmeteo")) {
      next.remove("key");
      next.remove("host");
    }
    DynamicJsonDocument previous(screenWeatherConfigBytes);
    previous.set(screenWeatherConfig);
    screenWeatherConfig.set(next);
    if (!screenSave()) {
      screenWeatherConfig.set(previous);
      return "storage_error";
    }
    screenWeatherGeneration++;
    screenWeather = {};
    screenWeatherStockPending = true;
    screenWeatherRetry = {};
    screenWeatherRefresh = true;
    screenWeatherError = "";
    return "ok";
  }
  if (action == "screen.weather.refresh") {
    if (params.size())
      return "invalid_params";
    if (!(screenWeatherConfig["enabled"] | false))
      return "not_ready";
    if (screenWeatherBusy || !screenWeatherRetry.due(millis(),true))
      return "busy";
    screenWeatherRefresh = true;
    return "accepted";
  }
  if (action == "screen.update.get") {
    if (params.size())
      return "invalid_params";
    screenServicesStatus(data);
    return "ok";
  }
  if (action == "screen.update.install") {
    if (params.size() != 1 || !params["sha256"].is<const char *>())
      return "invalid_params";
    if (String(params["sha256"].as<const char *>()) != screenHex(screenImage.sha, 32))
      return "invalid_firmware";
    if (!screenGuardAvailable() || !screenGuard.activation ||
        strcmp(screenImage.state, "staged_verified"))
      return "not_ready";
    const char *result = screenGuardRequest(GUARD_INSTALL, 0, 0, data);
    if (!strcmp(result, "accepted"))
      screenImage.state = "installing";
    return result;
  }
  if (action == "screen.update.cancel") {
    if (params.size())
      return "invalid_params";
    screenImage.active = false;
    screenImage.pending = false;
    screenImage.state = "cancelled";
    screenSend("+download_bin:ERROR<0070>\r\n");
    return "ok";
  }
  if (action == "screen.update.start")
    return screenUpdateStart(params);
  return "unsupported_action";
}

static const char *screenServicesCommand(const String &action,
                                        JsonObjectConst params,
                                        JsonObject data) {
  if (action.startsWith("screen.link.") || action.startsWith("screen.media.") ||
      action.startsWith("screen.push.") || action.startsWith("screen.stream."))
    return screenMediaCommand(action, params, data);
  return screenLegacyServicesCommand(action, params, data);
}

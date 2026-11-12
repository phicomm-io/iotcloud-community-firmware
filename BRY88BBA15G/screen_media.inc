/* Transient media owner; all UART access remains in the application loop. */
extern "C" {
#include "protocol/media_wire.h"
}
static void screenRelayPacket(unsigned, uint32_t, const uint8_t *, size_t);
static void screenRelayPoll(uint32_t);
static void screenRelayStop();
static struct {
  media_parser parser = {};
  uint32_t session = 0, client = 0, seen = 0, ping = 0, rate = 115200,
           desired = 115200, maximum = 921600;
  uint32_t target = 0, phaseAt = 0, deadline = 0, sequence = 0, request = 0,
           sent = 0;
  uint32_t fallback = 0, retries = 0, frames = 0;
  unsigned mode = 0, phase = 0, failedPhase = 0, attempts = 0, autoIndex = 0;
  bool automatic = false, ready = false, attempted = false, pending = false;
  esp_pm_lock_handle_t apb = nullptr, sleep = nullptr;
  bool locked = false;
  uint8_t command[MEDIA_PACKET_MAX] = {},
          snapshot[MEDIA_STATUS_BYTES + MEDIA_STREAM_STATUS_BYTES + MEDIA_DECODER_DETAIL_BYTES] = {},
          streamConfig[519] = {};
  size_t commandSize = 0;
  uint32_t configuredSession = 0, lastId = 0;
  unsigned result = MEDIA_OFFLINE, lastOp = 0;
  const char *state = "unavailable", *requestState = "idle";
} screenMedia;
static bool screenMediaWrite(unsigned channel, uint32_t seq, const void *p,
                             size_t n) {
  uint8_t wire[2 * (MEDIA_PACKET_MAX + 18) + 2];
  size_t size =
      media_encode(wire, sizeof(wire), channel, screenMedia.session, seq, p, n);
  return size && screenUartWrite(wire, size) == size;
}
static bool screenMediaBaud(uint32_t rate) {
  if (!media_rate_valid(rate) ||
      uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(200)) != ESP_OK)
    return false;
  bool high = rate > 115200;
  if (high && !screenMedia.locked) {
    ESP_ERROR_CHECK(esp_pm_lock_acquire(screenMedia.apb));
    ESP_ERROR_CHECK(esp_pm_lock_acquire(screenMedia.sleep));
    screenMedia.locked = true;
  }
  uart_config_t serial = {};
  serial.baud_rate = rate;
  serial.data_bits = UART_DATA_8_BITS;
  serial.parity = UART_PARITY_DISABLE;
  serial.stop_bits = UART_STOP_BITS_1;
  serial.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  serial.source_clk = high ? UART_SCLK_APB : UART_SCLK_REF_TICK;
  bool ok = uart_param_config(UART_NUM_1, &serial) == ESP_OK;
  if (!high && screenMedia.locked) {
    ESP_ERROR_CHECK(esp_pm_lock_release(screenMedia.sleep));
    ESP_ERROR_CHECK(esp_pm_lock_release(screenMedia.apb));
    screenMedia.locked = false;
  }
  if (ok)
    screenMedia.rate = rate;
  screenMedia.parser.active = false;
  return ok;
}
static void screenMediaFallback() {
  bool retry = screenMedia.phase && screenMedia.automatic && screenMedia.mode;
  screenMedia.failedPhase = screenMedia.phase;
  screenRelayStop();
  screenMediaBaud(115200);
  screenMedia.phase = 0;
  if (retry) {
    screenMedia.autoIndex++;
    screenMedia.attempted = false;
  }
  screenMedia.ready = false;
  screenMedia.fallback++;
  screenMedia.state = "fallback";
  screenMedia.deadline = millis() + 3000;
  if (screenMedia.pending) {
    screenMedia.pending = false;
    screenMedia.requestState = "uncertain";
  }
}
static void screenMediaSnapshot(const uint8_t *p, size_t n) {
  if (n == 172) {
    /* Transitional media firmware exposed four notices and no view selector. */
    memset(screenMedia.snapshot, 0, sizeof(screenMedia.snapshot));
    memcpy(screenMedia.snapshot, p, 22);
    screenMedia.snapshot[28] = screenMedia.snapshot[29] = 4;
    memcpy(screenMedia.snapshot + MEDIA_STATUS_IMAGES, p + 24, 68);
    memcpy(screenMedia.snapshot + MEDIA_STATUS_HISTORY, p + 92, 40);
    memcpy(screenMedia.snapshot + MEDIA_STATUS_BYTES, p + 132, 40);
    unsigned slot = 0;
    for (unsigned i = 0; i < 8; i++) {
      const uint8_t *item = p + 92 + i * 5;
      if (item[4] == PUSH_QUEUED || item[4] == PUSH_DISPLAYED) {
        uint8_t *q = screenMedia.snapshot + MEDIA_STATUS_QUEUE + slot++ * 9;
        memcpy(q, item, 4);
        q[8] = item[4];
      }
    }
    screenMedia.snapshot[23] = slot;
  } else if (n == MEDIA_STATUS_BYTES + MEDIA_STREAM_STATUS_BYTES ||
             n == sizeof(screenMedia.snapshot)) {
    memset(screenMedia.snapshot, 0, sizeof(screenMedia.snapshot));
    memcpy(screenMedia.snapshot, p, n);
  } else {
    return;
  }
  invalidateBusinessStatusCache();
}
static void screenMediaPacket(void *, unsigned channel, uint32_t session,
                              uint32_t sequence, const uint8_t *p, size_t n) {
  if (session != screenMedia.session || !n)
    return;
  screenMedia.frames++;
  if (channel == MEDIA_LINK) {
    if (p[0] == MEDIA_PONG && n >= 6) {
      if (media_u32(p + 1) != screenMedia.rate)
        return;
      screenMedia.seen = millis();
      if (!screenMedia.phase && !p[5]) {
        screenMedia.ready = true;
        screenMedia.state = "ready";
      }
      if (n > 6)
        screenMediaSnapshot(p + 6, n - 6);
    } else if (p[0] == MEDIA_BAUD_READY && n == 5 && screenMedia.phase == 1 &&
               media_u32(p + 1) == screenMedia.target) {
      screenMedia.phase = 2;
      screenMedia.phaseAt = millis() + 150;
    } else if (p[0] == MEDIA_BAUD_PROBE && n == 65 && screenMedia.phase == 3) {
      for (unsigned i = 1; i < 65; i++)
        if (p[i] != (uint8_t)(i * 73u + 19u))
          return;
      uint8_t confirm[5] = {MEDIA_BAUD_CONFIRM};
      media_put32(confirm + 1, screenMedia.rate);
      screenMediaWrite(MEDIA_LINK, 0, confirm, sizeof(confirm));
      screenMedia.phase = 4;
    } else if (p[0] == MEDIA_BAUD_CONFIRM && n == 5 && screenMedia.phase == 4 &&
               media_u32(p + 1) == screenMedia.rate) {
      screenMedia.phase = 0;
      screenMedia.ready = true;
      screenMedia.state = "ready";
      screenMedia.seen = millis();
    }
    return;
  }
  screenMedia.seen = millis();
  if (channel == MEDIA_REPLY) {
    if (n < 2)
      return;
    if (screenMedia.pending && sequence == screenMedia.request &&
        p[0] == screenMedia.command[0]) {
      screenMedia.pending = false;
      screenMedia.result = p[1];
      if (p[0] == MEDIA_STREAM_CONFIG)
        screenMedia.configuredSession = screenMedia.session;
      screenMedia.requestState = p[1] ? "rejected" : "complete";
    }
    if (n > 2)
      screenMediaSnapshot(p + 2, n - 2);
  } else
    screenRelayPacket(channel, sequence, p, n);
}
static bool screenMediaFeed(unsigned c, uint32_t now) {
  if (screenOtaActive())
    return false;
  return media_feed(&screenMedia.parser, c, now);
}
static bool screenMediaAnnounce(const String &arg) {
  unsigned version, session, mode, mask, rate;
  char extra;
  if (sscanf(arg.c_str(), "%u %8x %u %u %u%c", &version, &session, &mode, &mask,
             &rate, &extra) != 5 ||
      version != 1 || !session || mode > 3 || mask != 15 ||
      !media_rate_valid(rate))
    return true;
  if (screenMedia.session != session) {
    screenRelayStop();
    screenMedia.session = session;
    screenMedia.ready = false;
    screenMedia.attempted = false;
    screenMedia.configuredSession = 0;
    screenMedia.sequence = screenMedia.request = 0;
    screenMedia.autoIndex = 0;
    memset(screenMedia.snapshot, 0, sizeof(screenMedia.snapshot));
    if (screenMedia.pending) {
      screenMedia.pending = false;
      screenMedia.requestState = "uncertain";
    }
  }
  screenMedia.mode = mode;
  screenMedia.seen = millis();
  if (!screenMedia.phase && !screenMedia.ready)
    screenMedia.state = "synchronizing";
  return true;
}
static void screenMediaInit() {
  screenMedia.streamConfig[1] = 64;
  screenMedia.streamConfig[2] = 1;
  screenMedia.streamConfig[3] = 240;
  screenMedia.streamConfig[5] = 1;
  screenMedia.streamConfig[6] = 2;
  screenMedia.client = esp_random();
  if (!screenMedia.client)
    screenMedia.client = 1;
  screenMedia.parser.packet = screenMediaPacket;
  ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "media_uart",
                                     &screenMedia.apb));
  ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "media_awake",
                                     &screenMedia.sleep));
  Preferences saved;
  if (saved.begin("g3media", true)) {
    screenMedia.automatic = saved.getBool("auto", false);
    uint32_t desired = saved.getUInt("rate", 115200),
             maximum = saved.getUInt("maximum", 921600);
    if (media_rate_valid(desired))
      screenMedia.desired = desired;
    if (media_rate_valid(maximum))
      screenMedia.maximum = maximum;
    if (saved.getBytesLength("stream") == sizeof(screenMedia.streamConfig))
      saved.getBytes("stream", screenMedia.streamConfig,
                     sizeof(screenMedia.streamConfig));
    saved.end();
  }
}
static bool screenMediaNegotiating() {
  return screenMedia.phase || (screenMedia.mode && !screenMedia.attempted);
}
static bool screenMediaAvailable() {
  return screenMedia.ready && screenMedia.session &&
         uint32_t(millis() - screenMedia.seen) < 10000;
}
static const char *screenMediaSubmit(const uint8_t *p, size_t n,
                                     JsonObject out) {
  if (!screenMediaAvailable())
    return "not_ready";
  if (screenOtaActive() || screenImage.active || screenUploadBusy ||
      screenFontBusy() || screenWallpaper.active || screenWallpaper.pending)
    return "busy";
  if (n > sizeof(screenMedia.command) || !n)
    return "invalid_params";
  if (screenMedia.pending) {
    if (screenMedia.commandSize != n || memcmp(screenMedia.command, p, n))
      return "busy";
  } else {
    memcpy(screenMedia.command, p, n);
    screenMedia.commandSize = n;
    screenMedia.request = ++screenMedia.sequence;
    if (!screenMedia.request)
      return "not_ready";
    screenMedia.lastOp = p[0];
    screenMedia.lastId = n >= 5 ? media_u32(p + 1) : 0;
    screenMedia.pending = true;
    screenMedia.requestState = "pending";
    screenMedia.result = MEDIA_OFFLINE;
    screenMedia.attempts = 0;
    screenMedia.sent = 0;
  }
  out["request"] = screenMedia.request;
  out["id"] = screenMedia.lastId;
  out["state"] = screenMedia.requestState;
  return "accepted";
}
static void screenLinkGet(JsonObject out) {
  out["available"] = screenMediaAvailable();
  out["mode"] = screenMedia.automatic ? "auto" : "fixed";
  out["rate"] = screenMedia.desired;
  out["max_rate"] = screenMedia.maximum;
  out["actual_rate"] = screenMedia.rate;
  uint32_t hardware = 0;
  uart_get_baudrate(UART_NUM_1, &hardware);
  out["hardware_rate"] = hardware;
  out["apb_hz"] = getApbFrequency();
  out["negotiation_phase"] = screenMedia.phase;
  out["last_failed_phase"] = screenMedia.failedPhase;
  out["state"] = screenMedia.state;
  out["session"] = screenMedia.session;
  out["crc_errors"] = screenMedia.parser.crc_errors;
  out["framing_errors"] = screenMedia.parser.framing_errors;
  out["retries"] = screenMedia.retries;
  out["fallbacks"] = screenMedia.fallback;
  out["frames"] = screenMedia.frames;
  JsonArray rates = out.createNestedArray("rates");
  for (unsigned rate : {115200u, 230400u, 460800u, 921600u})
    rates.add(rate);
}
static const char *screenLinkSet(JsonObjectConst params, JsonObject out) {
  if (!screenFields(params, "|mode||rate||max_rate||apply_now|") ||
      !params["mode"].is<const char *>())
    return "invalid_params";
  String mode = params["mode"].as<String>();
  uint32_t rate = params["rate"] | screenMedia.desired,
           maximum = params["max_rate"] | screenMedia.maximum;
  if ((mode != "auto" && mode != "fixed") || !media_rate_valid(rate) ||
      !media_rate_valid(maximum) ||
      (params.containsKey("rate") && !params["rate"].is<uint32_t>()) ||
      (params.containsKey("max_rate") && !params["max_rate"].is<uint32_t>()))
    return "invalid_params";
  if (params.containsKey("apply_now") && !params["apply_now"].is<bool>())
    return "invalid_params";
  if ((params["apply_now"] | false) &&
      !(screenBridge.capabilities & SCREEN_CAP_BINARY_LINK))
    return "not_ready";
  Preferences saved;
  if (!saved.begin("g3media", false))
    return "storage_error";
  bool ok = saved.putUInt("rate", rate) == 4 &&
            saved.putUInt("maximum", maximum) == 4 &&
            saved.putBool("auto", mode == "auto") == 1;
  saved.end();
  if (!ok)
    return "storage_error";
  screenMedia.desired = rate;
  screenMedia.maximum = maximum;
  screenMedia.automatic = mode == "auto";
  screenMedia.attempted = false;
  if (params["apply_now"] | false) {
    screenMedia.attempted = true;
    return screenGuardRequest(GUARD_RESTART, 0, 0, out);
  }
  screenLinkGet(out);
  out["apply"] = screenMedia.mode ? "recovery_window" : "next_recovery";
  return "ok";
}
static const char *screenPushState(unsigned state) {
  static const char *const names[] = {"empty",     "queued",   "displayed",
                                      "confirmed", "timedout", "dismissed",
                                      "failed"};
  return state < 7 ? names[state] : "unknown";
}
static void screenMediaGet(JsonObject out, bool all = true) {
  out["available"] = screenMediaAvailable();
  out["active_id"] = media_u32(screenMedia.snapshot);
  out["displayed"] = screenMedia.snapshot[20] != 0;
  out["suspended"] = screenMedia.snapshot[21] != 0;
  out["decoded_images"] = media_u32(screenMedia.snapshot + 8);
  out["decode_errors"] = media_u32(screenMedia.snapshot + 12);
  out["pixel_crc32"] = media_u32(screenMedia.snapshot + 16);
  out["view_index"] = screenMedia.snapshot[22];
  out["queued"] = screenMedia.snapshot[23];
  out["decoder_status"] = screenMedia.snapshot[24];
  out["decoder_flags"] = screenMedia.snapshot[25];
  out["decoder_ms"] = screenMedia.snapshot[26] | screenMedia.snapshot[27] << 8;
  JsonArray decoder = out.createNestedArray("decoder_registers");
  for (unsigned i = 0; i < MEDIA_DECODER_DETAIL_BYTES / 4; i++)
    decoder.add(media_u32(screenMedia.snapshot + MEDIA_STATUS_BYTES + MEDIA_STREAM_STATUS_BYTES + i * 4));
  JsonObject request = out.createNestedObject("request");
  request["sequence"] = screenMedia.request;
  request["id"] = screenMedia.lastId;
  request["operation"] = screenMedia.lastOp;
  request["state"] = screenMedia.requestState;
  request["result"] = screenMedia.result;
  if (!all)
    return;
  out["max_bytes"] = MEDIA_JPEG_MAX;
  out["max_width"] = MEDIA_WIDTH_MAX;
  out["max_height"] = MEDIA_HEIGHT_MAX;
  out["chunk_bytes"] = MEDIA_CHUNK;
  out["queue_capacity"] = screenMedia.snapshot[28];
  out["image_capacity"] = MEDIA_IMAGE_SLOTS;
  out["stack_capacity"] = screenMedia.snapshot[30];
  JsonArray images = out.createNestedArray("images"),
            queue = out.createNestedArray("queue"),
            messages = out.createNestedArray("messages");
  for (unsigned i = 0; i < MEDIA_IMAGE_SLOTS; i++) {
    const uint8_t *p = screenMedia.snapshot + MEDIA_STATUS_IMAGES + 17 * i;
    if (!media_u32(p))
      continue;
    JsonObject image = images.createNestedObject();
    image["id"] = media_u32(p);
    image["total"] = media_u32(p + 4);
    image["offset"] = media_u32(p + 8);
    image["crc32"] = media_u32(p + 12);
    image["state"] = p[16] == 2 ? "ready" : "uploading";
  }
  for (unsigned i = 0; i < MEDIA_QUEUE; i++) {
    const uint8_t *p = screenMedia.snapshot + MEDIA_STATUS_QUEUE + 9 * i;
    if (!media_u32(p))
      continue;
    JsonObject msg = queue.createNestedObject();
    msg["id"] = media_u32(p);
    msg["image_id"] = media_u32(p + 4);
    msg["state"] = screenPushState(p[8]);
    msg["view_index"] = i < screenMedia.snapshot[30] ? i + 1 : 0;
  }
  for (unsigned i = 0; i < MEDIA_HISTORY; i++) {
    const uint8_t *p = screenMedia.snapshot + MEDIA_STATUS_HISTORY + 5 * i;
    if (!media_u32(p))
      continue;
    JsonObject msg = messages.createNestedObject();
    msg["id"] = media_u32(p);
    msg["state"] = screenPushState(p[4]);
  }
}
static void screenStreamGet(JsonObject out) {
  const uint8_t *config = screenMedia.streamConfig,
                *p = screenMedia.snapshot + MEDIA_STATUS_BYTES;
  out["available"] = screenMediaAvailable();
  out["enabled"] = (config[6] & 1) != 0;
  out["url"] = (const char *)config + 7;
  out["username"] = (const char *)config + 391;
  out["password_set"] = config[455] != 0;
  out["verify_tls"] = (config[6] & 2) != 0;
  out["width"] = config[1] | config[2] << 8;
  out["height"] = config[3] | config[4] << 8;
  out["fps"] = config[5];
  static const char *const names[] = {"off", "paused", "connecting", "reading",
                                      "backoff"};
  out["state"] = p[0] < 5 ? names[p[0]] : "unknown";
  out["actual_width"] = p[2] | p[3] << 8;
  out["actual_height"] = p[4] | p[5] << 8;
  out["frames"] = media_u32(p + 8);
  out["dropped"] = media_u32(p + 12);
  out["bytes"] = media_u32(p + 16);
  out["pixel_crc32"] = media_u32(p + 20);
  out["error"] = media_u32(p + 24);
  out["actual_millifps"] = media_u32(p + 28);
  out["connection"] = media_u32(p + 36);
}
/* Match the MCU URL grammar before persisting a source. Placeholders are
 * expanded only in the path, which keeps DNS/port validation deterministic. */
static bool screenMediaString(JsonVariantConst value, size_t limit) {
  if (!value.is<const char *>())
    return false;
  JsonString text = value.as<JsonString>();
  return text.size() <= limit && strlen(text.c_str()) == text.size();
}
static bool screenStreamUrl(const char *source) {
  const char *host = source;
  if (!strncmp(host, "http://", 7))
    host += 7;
  else if (!strncmp(host, "https://", 8))
    host += 8;
  else
    return false;
  unsigned count = 0;
  while (*host && *host != '/' && *host != ':' && *host != '?') {
    unsigned char c = *host++;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '-') ||
        ++count > 127)
      return false;
  }
  if (!count)
    return false;
  if (*host == ':') {
    unsigned port = 0;
    host++;
    if (*host < '0' || *host > '9')
      return false;
    while (*host >= '0' && *host <= '9') {
      port = port * 10 + *host++ - '0';
      if (port > 65535)
        return false;
    }
    if (!port)
      return false;
  }
  if (*host && *host != '/' && *host != '?')
    return false;
  for (const char *c = source; *c; c++)
    if ((unsigned char)*c <= 32 || (unsigned char)*c == 127 || *c == '#')
      return false;
  return true;
}
static const char *screenMediaCommand(const String &action,
                                      JsonObjectConst params, JsonObject out) {
  if (action == "screen.link.get") {
    if (params.size())
      return "invalid_params";
    screenLinkGet(out);
    return "ok";
  }
  if (action == "screen.link.set")
    return screenLinkSet(params, out);
  if (action == "screen.media.get" || action == "screen.push.get") {
    if (params.size())
      return "invalid_params";
    screenMediaGet(out);
    return "ok";
  }
  if (action == "screen.stream.get") {
    if (params.size())
      return "invalid_params";
    screenStreamGet(out);
    return "ok";
  }
  uint8_t p[MEDIA_PACKET_MAX] = {};
  size_t n = 0;
  if (action == "screen.media.begin") {
    if (params.size() != 5 ||
        !screenFields(params, "|id||total||crc32||width||height|"))
      return "invalid_params";
    for (const char *key : {"id", "total", "crc32", "width", "height"})
      if (!params[key].is<uint32_t>())
        return "invalid_params";
    uint32_t id = params["id"], size = params["total"], w = params["width"],
             h = params["height"];
    if (!id || !size || size > MEDIA_JPEG_MAX || !w || w > MEDIA_WIDTH_MAX ||
        !h || h > MEDIA_HEIGHT_MAX)
      return "invalid_params";
    p[0] = MEDIA_BEGIN;
    media_put32(p + 1, id);
    media_put32(p + 5, size);
    media_put32(p + 9, params["crc32"]);
    p[13] = w;
    p[14] = w >> 8;
    p[15] = h;
    p[16] = h >> 8;
    n = 17;
  } else if (action == "screen.media.chunk") {
    if (params.size() != 3 || !screenFields(params, "|id||offset||bytes|") ||
        !params["id"].is<uint32_t>() || !params["id"].as<uint32_t>() ||
        !params["offset"].is<uint32_t>())
      return "invalid_params";
    p[0] = MEDIA_WRITE;
    media_put32(p + 1, params["id"]);
    media_put32(p + 5, params["offset"]);
    size_t bytes = 0;
    if (!screenUnbase64(params["bytes"], p + 9, MEDIA_CHUNK, bytes) || !bytes)
      return "invalid_params";
    n = 9 + bytes;
  } else if (action == "screen.media.commit" ||
             action == "screen.media.cancel" ||
             action == "screen.push.dismiss") {
    if (params.size() != 1 || !params["id"].is<uint32_t>() ||
        !params["id"].as<uint32_t>())
      return "invalid_params";
    p[0] = action.endsWith("commit")   ? MEDIA_COMMIT
           : action.endsWith("cancel") ? MEDIA_CANCEL
                                       : MEDIA_DISMISS;
    media_put32(p + 1, params["id"]);
    n = 5;
  } else if (action == "screen.push.select") {
    if (params.size() != 1 || !params["index"].is<unsigned>() ||
        params["index"].as<unsigned>() > MEDIA_STACK)
      return "invalid_params";
    if (!screenMedia.snapshot[30])
      return "unsupported";
    p[0] = MEDIA_SELECT;
    p[1] = params["index"].as<unsigned>();
    n = 2;
  } else if (action == "screen.push.send") {
    if (!screenFields(params, "|id||image_id||title||text||duration_ms||mode||"
                              "wake||brightness|") ||
        !params["id"].is<uint32_t>() || !params["id"].as<uint32_t>())
      return "invalid_params";
    const char *title = params["title"] | "", *text = params["text"] | "",
               *mode = params["mode"] | "timed";
    if ((params.containsKey("title") &&
         !screenMediaString(params["title"], 64)) ||
        (params.containsKey("text") &&
         !screenMediaString(params["text"], 512)) ||
        (params.containsKey("mode") && !screenMediaString(params["mode"], 7)) ||
        strlen(title) > 64 || strlen(text) > 512 || !scene_utf8(title, 65) ||
        !scene_text_utf8(text, 513) ||
        (strcmp(mode, "timed") && strcmp(mode, "confirm")))
      return "invalid_params";
    uint32_t id = params["id"], image = params["image_id"] | 0u,
             duration = params["duration_ms"] | 10000u;
    unsigned brightness = params["brightness"] | 30u;
    if ((params.containsKey("image_id") &&
         !params["image_id"].is<uint32_t>()) ||
        (params.containsKey("duration_ms") &&
         !params["duration_ms"].is<uint32_t>()) ||
        (params.containsKey("wake") && !params["wake"].is<bool>()) ||
        (params.containsKey("brightness") &&
         !params["brightness"].is<unsigned>()) ||
        !brightness || brightness > 100 || duration > 3600000 ||
        (!strcmp(mode, "timed") && !duration) ||
        (!image && !title[0] && !text[0]))
      return "invalid_params";
    unsigned count = 0;
    bool known = false;
    for (unsigned i = 0; i < MEDIA_HISTORY; i++) {
      const uint8_t *item = screenMedia.snapshot + MEDIA_STATUS_HISTORY + 5 * i;
      if (media_u32(item) == id)
        known = true;
    }
    count = screenMedia.snapshot[23];
    if (!known && count >= screenMedia.snapshot[28])
      return "busy";
    p[0] = MEDIA_PUSH;
    media_put32(p + 1, id);
    media_put32(p + 5, image);
    media_put32(p + 9, duration);
    p[13] = (params["wake"] | true) ? 1 : 0;
    if (!strcmp(mode, "confirm"))
      p[13] |= 2;
    p[14] = brightness;
    p[15] = strlen(title);
    memcpy(p + 16, title, strlen(title));
    memcpy(p + 16 + strlen(title), text, strlen(text));
    n = 16 + strlen(title) + strlen(text);
  } else if (action == "screen.stream.set") {
    if (!screenFields(params, "|url||username||password||width||height||fps||"
                              "enabled||verify_tls|") ||
        !screenMediaString(params["url"], 383))
      return "invalid_params";
    const char *url = params["url"], *username = params["username"] | "",
               *password = params["password"] |
                           (const char *)(screenMedia.streamConfig + 455);
    unsigned w = params["width"] | 320u, h = params["height"] | 240u,
             fps = params["fps"] | 1u;
    if (!w || w > 480 || !h || h > 854 || !fps || fps > 10 ||
        !screenStreamUrl(url))
      return "invalid_params";
    for (const char *key : {"username", "password"}) {
      if (params.containsKey(key) && !screenMediaString(params[key], 63))
        return "invalid_params";
    }
    for (const char *field : {username, password})
      for (const char *c = field; *c; c++)
        if ((unsigned char)*c < 32 || (unsigned char)*c == 127)
          return "invalid_params";
    for (const char *key : {"width", "height", "fps"})
      if (params.containsKey(key) && !params[key].is<unsigned>())
        return "invalid_params";
    for (const char *key : {"enabled", "verify_tls"})
      if (params.containsKey(key) && !params[key].is<bool>())
        return "invalid_params";
    p[0] = MEDIA_STREAM_CONFIG;
    p[1] = w;
    p[2] = w >> 8;
    p[3] = h;
    p[4] = h >> 8;
    p[5] = fps;
    p[6] = (params["enabled"] | true) ? 1 : 0;
    if (params["verify_tls"] | true)
      p[6] |= 2;
    memcpy(p + 7, url, strlen(url));
    memcpy(p + 391, username, strlen(username));
    memcpy(p + 455, password, strlen(password));
    n = 519;
    if (screenMedia.pending)
      return "busy";
    Preferences saved;
    if (!saved.begin("g3media", false))
      return "storage_error";
    bool ok = saved.putBytes("stream", p, n) == n;
    saved.end();
    if (!ok)
      return "storage_error";
    memcpy(screenMedia.streamConfig, p, n);
    screenMedia.configuredSession = 0;
  } else if (action == "screen.stream.start" ||
             action == "screen.stream.stop") {
    if (params.size())
      return "invalid_params";
    if (!screenMedia.streamConfig[0])
      return "not_ready";
    if (screenMedia.pending)
      return "busy";
    memcpy(p, screenMedia.streamConfig, sizeof(screenMedia.streamConfig));
    if (action.endsWith("start"))
      p[6] |= 1;
    else
      p[6] &= ~1u;
    Preferences saved;
    if (!saved.begin("g3media", false))
      return "storage_error";
    bool ok = saved.putBytes("stream", p, sizeof(screenMedia.streamConfig)) ==
              sizeof(screenMedia.streamConfig);
    saved.end();
    if (!ok)
      return "storage_error";
    memcpy(screenMedia.streamConfig, p, sizeof(screenMedia.streamConfig));
    n = 519;
    screenMedia.configuredSession = 0;
  } else
    return "not_supported";
  const char *code = screenMediaSubmit(p, n, out);
  return code;
}
static void screenMediaPoll(uint32_t now) {
  /* RX callbacks above can advance seen after screenPoll captured its tick.
   * Refresh here so unsigned age arithmetic cannot treat that as a timeout. */
  now = millis();
  screenRelayPoll(now);
  if (!screenMedia.session || screenOtaActive())
    return;
  if ((screenMedia.phase && (int32_t)(now - screenMedia.deadline) >= 0) ||
      (screenMedia.rate != 115200 &&
       uint32_t(now - screenMedia.seen) > 10000)) {
    screenMediaFallback();
    return;
  }
  if (screenMedia.phase == 2 && (int32_t)(now - screenMedia.phaseAt) >= 0) {
    if (!screenMediaBaud(screenMedia.target)) {
      screenMediaFallback();
      return;
    }
    uint8_t p[65] = {MEDIA_BAUD_PROBE};
    for (unsigned i = 1; i < 65; i++)
      p[i] = i * 73u + 19u;
    screenMediaWrite(MEDIA_LINK, 0, p, sizeof(p));
    screenMedia.phase = 3;
  }
  if (screenMedia.phase)
    return;
  if (screenMedia.mode && !screenMedia.attempted &&
      (!screenMedia.deadline || (int32_t)(now - screenMedia.deadline) >= 0) &&
      !screenImage.active) {
    uint32_t target = screenMedia.desired;
    if (screenMedia.automatic) {
      static const unsigned rates[] = {921600, 460800, 230400, 115200};
      while (screenMedia.autoIndex < 4 &&
             rates[screenMedia.autoIndex] > screenMedia.maximum)
        screenMedia.autoIndex++;
      target = rates[screenMedia.autoIndex < 4 ? screenMedia.autoIndex : 3];
    }
    screenMedia.target = target;
    screenMedia.attempted = true;
    if (target != screenMedia.rate) {
      uint8_t p[5] = {MEDIA_BAUD};
      media_put32(p + 1, target);
      screenMediaWrite(MEDIA_LINK, 0, p, sizeof(p));
      screenMedia.phase = 1;
      screenMedia.deadline = now + 2200;
      screenMedia.ready = false;
      screenMedia.state = "negotiating";
      return;
    }
  }
  if (now - screenMedia.ping >= 2000) {
    uint8_t p[5] = {MEDIA_PING};
    media_put32(p + 1, screenMedia.client);
    screenMediaWrite(MEDIA_LINK, 0, p, sizeof(p));
    screenMedia.ping = now;
  }
  if (screenMedia.pending) {
    if (!screenMedia.sent || uint32_t(now - screenMedia.sent) > 1500) {
      if (screenMedia.attempts >= 3) {
        screenMedia.pending = false;
        screenMedia.requestState = "uncertain";
        return;
      }
      if (screenMediaWrite(MEDIA_COMMAND, screenMedia.request,
                           screenMedia.command, screenMedia.commandSize)) {
        if (screenMedia.attempts)
          screenMedia.retries++;
        screenMedia.attempts++;
        screenMedia.sent = now;
      }
    }
  } else if (screenMediaAvailable() &&
             screenMedia.streamConfig[0] == MEDIA_STREAM_CONFIG &&
             screenMedia.configuredSession != screenMedia.session) {
    StaticJsonDocument<128> reply;
    const char *code = screenMediaSubmit(screenMedia.streamConfig,
                                         sizeof(screenMedia.streamConfig),
                                         reply.to<JsonObject>());
    (void)code;
  }
}

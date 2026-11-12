/* Staging is available independently of display initialization. */
static String screenOtaError;
static const char *screenOtaErrorText() { return screenOtaError.c_str(); }
static volatile bool screenUploadBusy = false, screenArtifactPending = false;
static uint8_t screenArtifactHeader[BRY_IMAGE_HEADER_BYTES];
static portMUX_TYPE screenImageMux = portMUX_INITIALIZER_UNLOCKED;
static bool screenOtaActive() { return screenImage.active; }
static bool screenOtaCommandAllowed(const String &cmd) {
  return !screenOtaActive() || cmd == "bin_summary" || cmd == "start_ota" ||
         cmd == "download_bin" || cmd == ">" || cmd == "end_ota" ||
         cmd == "cancel_ota" || cmd == "screen_ota_error" ||
         cmd == "screen_image" || cmd == "screen_recovery";
}
static void screenOtaReplyError(const String &cmd, unsigned error) {
  if (screenOtaError.length()) return;
  // Report only a bounded command name, never peer arguments or payload bytes.
  bool valid = cmd.length() && cmd.length() <= 32;
  for (size_t i = 0; i < cmd.length(); ++i)
    valid = valid && ((cmd.c_str()[i] >= 'a' && cmd.c_str()[i] <= 'z') || cmd.c_str()[i] == '_');
  char code[16];
  snprintf(code, sizeof(code), "_%04X", error);
  screenOtaError = String("reply_") + (valid ? cmd : String("unknown")) + code;
}
/* Binary OTA owns this UART until its transfer ends. In particular, a weather
 * replay between download_bin metadata and the raw packet corrupts the MCU's
 * length-driven receiver. Business writers must retry after OTA. */
static bool screenBusinessSend(const char *bytes, size_t size) {
  return size <= 3072 && !screenImage.active &&
         screenUartWrite((const uint8_t *)bytes, size) == size;
}
static String screenHex(const uint8_t *p, size_t n) {
  String s;
  s.reserve(n * 2);
  const char *digits = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    s += digits[p[i] >> 4];
    s += digits[p[i] & 15];
  }
  return s;
}
static bool screenHash(uint8_t md5[16], uint8_t sha[32]) {
  mbedtls_md5_context m;
  mbedtls_sha256_context s;
  mbedtls_md5_init(&m);
  mbedtls_sha256_init(&s);
  mbedtls_md5_starts_ret(&m);
  mbedtls_sha256_starts_ret(&s, 0);
  uint8_t bytes[1024];
  bool ok = true;
  for (uint32_t at = 0; at < BRY_IMAGE_LIMIT; at += sizeof(bytes)) {
    size_t n = min((uint32_t)sizeof(bytes), BRY_IMAGE_LIMIT - at);
    if (!screenReadStore(nullptr, BRY_IMAGE_START + at, bytes, n)) {
      ok = false;
      break;
    }
    if (mbedtls_md5_update_ret(&m, bytes, n) ||
        mbedtls_sha256_update_ret(&s, bytes, n)) {
      ok = false;
      break;
    }
    delay(1);
  }
  ok = ok && !mbedtls_md5_finish_ret(&m, md5) &&
       !mbedtls_sha256_finish_ret(&s, sha);
  mbedtls_md5_free(&m);
  mbedtls_sha256_free(&s);
  return ok;
}
static bool screenImageVerify() {
  if (screenUploadBusy || !screenImage.valid)
    return false;
  uint8_t m[16], s[32];
  return screenHash(m, s) && !memcmp(m, screenImage.md5, 16) &&
         !memcmp(s, screenImage.sha, 32);
}
static void screenImportArtifact() {
  if (screenUploadBusy || !screenArtifactPending)
    return;
  uint8_t h[BRY_IMAGE_HEADER_BYTES];
  portENTER_CRITICAL(&screenImageMux);
  memcpy(h, screenArtifactHeader, sizeof(h));
  screenArtifactPending = false;
  portEXIT_CRITICAL(&screenImageMux);
  screenImage.valid = bry_image_header_decode(h, BRY_IMAGE_LIMIT, screenImage.md5,
                                              screenImage.sha, &screenImage.features);
  if (screenImage.valid) {
    screenImage.size = BRY_IMAGE_LIMIT;
    screenImage.state = "artifact_ready";
  } else
    screenImage.state = "upload_failed";
  invalidateBusinessStatusCache();
}
static bool screenOtaCommand(const String &cmd, const String &arg) {
  if (cmd == "screen_ota_error") {
    if (screenImage.active && !screenOtaError.length() && arg.length() <= 128) screenOtaError = arg;
    return true;
  }
  if (cmd == "screen_image")
    return screenGuardImageAnnounce(arg);
  if (cmd == "screen_recovery") {
    if (arg.startsWith("2 ") || arg.startsWith("3 "))
      return screenGuardAnnounce(arg);
    screenRecovering = arg == "1 staging-only";
    return true;
  }
  if (cmd == "bin_summary") {
    if (screenImage.active && arg.startsWith("1 ")) {
      if (screenImage.offset || screenImage.pending) {
        screenReply(cmd, false, 0x70);
        return true;
      }
      screenImage.offset = 0;
      screenImage.pending = false;
      screenImage.state = "waiting_for_start";
      /* Thirteen bounded 32-KiB erases can take up to 65 s on GPIO NOR. */
      screenImage.deadline = millis() + 90000;
      Serial.println("screen OTA metadata requested");
      screenSend("+bin_summary:SV.99.00.01.00," +
                 screenHex(screenImage.md5, 16) + "\r\n");
    } else
      screenSend("+bin_summary:OK\r\n");
    return true;
  }
  if (cmd != "start_ota" && cmd != "download_bin" && cmd != ">" &&
      cmd != "end_ota" && cmd != "cancel_ota")
    return false;
  if (!screenImage.active) {
    screenReply(cmd, false, 0x70);
    return true;
  }
  screenImage.deadline = millis() + 60000;
  if (cmd == "start_ota") {
    Serial.printf("screen OTA start: %s\n", arg.c_str());
    if (arg != "1 20" || screenImage.offset || screenImage.pending) {
      screenReply(cmd, false, 0x34);
      return true;
    }
    screenImage.state = "transferring";
    screenReply(cmd);
    return true;
  }
  if (cmd == "download_bin") {
    if (arg != "1024" || screenImage.pending) {
      screenReply(cmd, false, 0x70);
      return true;
    }
    if (screenImage.offset == screenImage.size) {
      String final = "+download_bin:END\r\n";
      /* Community receivers synchronously re-read GPIO NOR before end_ota.
       * Queue the final digest behind END so it is already in their UART
       * ring when that slow read finishes. This also supports the first
       * recovery image whose deadline was not rearmed after the read.
       * The MCU still verifies stream/NOR hashes before consuming this line.
       * The stock bootstrap keeps its original request/reply ordering. */
      if (!screenImage.bootstrap)
        final = final + "+end_ota:" + screenHex(screenImage.md5, 16) + "\r\n";
      screenSend(final);
    } else {
      screenImage.chunk = min(screenImage.bootstrap ? 512u : 1024u,
                              screenImage.size - screenImage.offset);
      screenImage.pending = true;
      screenSend("+download_bin:" + String(screenImage.offset) + "," +
                 String(screenImage.chunk) + "\r\n");
    }
    if (!(screenImage.offset & 0x3fff))
      Serial.printf("screen OTA offset %u/%u\n", screenImage.offset, screenImage.size);
    return true;
  }
  if (cmd == ">") {
    if (!screenImage.pending) {
      screenReply(cmd, false, 0x70);
      return true;
    }
    uint8_t bytes[5 + 1024] = {'+', 'b', 'i', 'n', ':'};
    if (!screenReadStore(nullptr, BRY_IMAGE_START + screenImage.offset, bytes + 5,
                         screenImage.chunk)) {
      screenImage.active = false;
      screenImage.state = "read_error";
      screenReply("download_bin", false, 0x70);
      return true;
    }
    // Factory 0x400dadd4 sends exactly five prefix bytes plus raw payload.
    // A trailing CR/LF overflows the stock MCU's 0x406-byte receive buffer.
    // Queue the whole packet in one write so task scheduling cannot insert
    // a receive-idle gap between the prefix and binary body.
    if (screenUartWrite(bytes, 5 + screenImage.chunk) != 5 + screenImage.chunk) {
      screenImage.active = false;
      screenImage.state = "write_error";
      return true;
    }
    screenImage.offset += screenImage.chunk;
    screenImage.pending = false;
    return true;
  }
  if (cmd == "end_ota") {
    Serial.printf("screen OTA end at %u/%u\n", screenImage.offset, screenImage.size);
    if (screenImage.offset != screenImage.size || screenImage.pending) {
      screenReply(cmd, false, 0x35);
      return true;
    }
    if (screenImage.bootstrap)
      screenSend("+end_ota:" + screenHex(screenImage.md5, 16) + "\r\n");
    screenImage.active = false;
    screenImage.state = screenImage.bootstrap ? "factory_installing" : "staged_verified";
    return true;
  }
  screenImage.active = false;
  screenImage.pending = false;
  screenImage.state = "cancelled";
  screenReply(cmd);
  return true;
}

static esp_err_t screenUploadHandler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_OK;
  char target[48] = {};
  if (httpd_req_get_hdr_value_str(req, "X-IotCloud-Target", target,
                                  sizeof(target)) != ESP_OK ||
      strcmp(target, "BRY88BBA15G-screen") ||
      req->content_len != BRY_IMAGE_LIMIT)
    return jsonResponse(req, "400 Bad Request",
                        "{\"code\":\"invalid_firmware\"}");
  if (!screenStoreAvailable || !screenStoreMutex ||
      xSemaphoreTake(screenStoreMutex, 0) != pdTRUE)
    return jsonResponse(req, "503 Service Unavailable", "{\"code\":\"busy\"}");
  bool busy;
  portENTER_CRITICAL(&screenImageMux);
  busy = g3OnlineBusy || otaActive || screenImage.active || screenUploadBusy || screenWallpaper.active || screenWallpaper.pending;
  if (!busy)
    screenUploadBusy = true;
  portEXIT_CRITICAL(&screenImageMux);
  if (busy) {
    xSemaphoreGive(screenStoreMutex);
    return jsonResponse(req, "503 Service Unavailable", "{\"code\":\"busy\"}");
  }
  bool powerReady = screenModulePowerBegin();
  if (!powerReady) {
    portENTER_CRITICAL(&screenImageMux);
    screenUploadBusy = false;
    portEXIT_CRITICAL(&screenImageMux);
    xSemaphoreGive(screenStoreMutex);
    return jsonResponse(req, "503 Service Unavailable", "{\"code\":\"busy\"}");
  }
  bool ok = bry_store_claim(&screenStore) &&
            screenEraseStore(nullptr, BRY_IMAGE_HEADER, 4096);
  ScreenUploadErase eraser(BRY_IMAGE_START, BRY_IMAGE_LIMIT,
                           screenStorePartition.size);
  mbedtls_md5_context md5;
  mbedtls_sha256_context sha;
  mbedtls_md5_init(&md5);
  mbedtls_sha256_init(&sha);
  mbedtls_md5_starts_ret(&md5);
  mbedtls_sha256_starts_ret(&sha, 0);
  uint8_t bytes[1024], h[BRY_IMAGE_HEADER_BYTES] = {}, vectors[8] = {};
  uint32_t at = 0;
  bool identity = false, vector = false, recovery = false, activation = false;
  String tail;
  const char *signature =
      "iotcloud-screen/BRY88BBA15G;application=0x14000;";
  while (ok && at < BRY_IMAGE_LIMIT) {
    if (!screenModulePowerHealthy()) { ok = false; break; }
    int n = httpd_req_recv(req, (char *)bytes,
                           min((uint32_t)sizeof(bytes), BRY_IMAGE_LIMIT - at));
    if (n <= 0) {
      ok = false;
      break;
    }
    if (at < 8)
      memcpy(vectors + at, bytes, min((unsigned)n, 8u - at));
    if (at + (unsigned)n >= 8) {
      uint32_t sp = scene_u32(vectors), reset = scene_u32(vectors + 4);
      vector = sp == 0x20010000u && (reset & 1) && reset >= 0x14001 &&
               reset < 0x7b000;
    }
    String probe = tail;
    for (int i = 0; i < n; i++)
      probe += (char)(bytes[i] ? bytes[i] : ' ');
    identity = identity || probe.indexOf(signature) >= 0;
    recovery = recovery || probe.indexOf("install=factory-uart;mode=recovery;") >= 0;
    activation = activation || probe.indexOf("install=factory-uart;") >= 0;
    tail = probe.substring(probe.length() > 128 ? probe.length() - 128 : 0);
    ok = eraser.prepare(at, n, [](uint32_t address, size_t size) {
           return screenEraseStore(nullptr, address, size);
         }) && screenWriteStore(nullptr, BRY_IMAGE_START + at, bytes, n) &&
         !mbedtls_md5_update_ret(&md5, bytes, n) &&
         !mbedtls_sha256_update_ret(&sha, bytes, n);
    at += n;
    delay(1);
  }
  uint8_t m[16], s[32], verifyM[16], verifyS[32];
  ok = ok && vector && identity && at == BRY_IMAGE_LIMIT &&
       !mbedtls_md5_finish_ret(&md5, m) && !mbedtls_sha256_finish_ret(&sha, s);
  mbedtls_md5_free(&md5);
  mbedtls_sha256_free(&sha);
  char auth[80] = {};
  ph_auth_context context;
  requestAuthorization(req, auth);
  bool stillAuthorized = localAuthReady && !configRecovery &&
                         ph_auth_http_check(&localAuth, auth, &context);
  ok = ok && stillAuthorized && screenHash(verifyM, verifyS) &&
       !memcmp(m, verifyM, 16) && !memcmp(s, verifyS, 32);
  if (ok) {
    bry_image_header_encode(h, BRY_IMAGE_LIMIT, m, s,
                            (recovery ? 1u : 0u) | (activation ? 2u : 0u));
    uint8_t check[BRY_IMAGE_HEADER_BYTES];
    ok = screenWriteStore(nullptr, BRY_IMAGE_HEADER, h, sizeof(h)) &&
         screenReadStore(nullptr, BRY_IMAGE_HEADER, check, sizeof(check)) &&
         !memcmp(h, check, sizeof(h));
  }
  if (!ok)
    memset(h, 0, sizeof(h));
  portENTER_CRITICAL(&screenImageMux);
  memcpy(screenArtifactHeader, h, sizeof(h));
  screenArtifactPending = true;
  screenUploadBusy = false;
  portEXIT_CRITICAL(&screenImageMux);
  xSemaphoreGive(screenStoreMutex);
  if (powerReady) screenModulePowerEnd();
  if (!stillAuthorized)
    return jsonResponse(req, "401 Unauthorized", "{\"code\":\"unauthorized\"}");
  if (!ok)
    return jsonResponse(req, "400 Bad Request",
                        "{\"code\":\"invalid_firmware\"}");
  DynamicJsonDocument response(768);
  response["code"] = "ok";
  response["data"]["sha256"] = screenHex(s, 32);
  response["data"]["md5"] = screenHex(m, 16);
  response["data"]["size"] = at;
  response["data"]["installable"] = false;
  String wire;
  serializeWireMP(response, wire);
  return jsonResponse(req, "200 OK", wire);
}

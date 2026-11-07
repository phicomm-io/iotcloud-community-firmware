// One application-loop owner. SDK SNTP runs asynchronously; never wait for
// DNS or a time response on the UART / recovery service path.
static uint32_t screenClockAddress, screenClockSyncAt, screenClockMcuAt,
    screenClockMcuUtc;
static bool screenClockOnline, screenClockSynced;

static void screenClockPoll(uint32_t now) {
  uint32_t address = (uint32_t)WiFi.localIP();
  bool online = WiFi.status() == WL_CONNECTED && address != 0;
  if (online && (!screenClockOnline || address != screenClockAddress)) {
    // Starting before DHCP can leave the first request in the SDK's retry
    // backoff. Restart on a new connection, with DNS and an IPv4 address ready.
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
    screenClockAddress = address;
  }
  screenClockOnline = online;
  if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
    screenClockSynced = true;
    screenClockSyncAt = now;
  }
}

static void screenClockSnapshot(JsonVariantConst stamp, uint32_t now) {
  if (!stamp.is<double>())
    return;
  double ms = stamp.as<double>();
  if (ms == 0) {
    screenClockMcuUtc = 0; // The MCU has explicitly reported no valid clock.
    return;
  }
  if (!isfinite(ms) || ms < 1700000000000.0 || ms >= 4294967296000.0)
    return;
  screenClockMcuUtc = (uint32_t)(ms / 1000);
  screenClockMcuAt = now;
}

static void screenClockStatus(JsonObject out, int timezone) {
  time_t utc = time(nullptr);
  bool valid = utc >= 1700000000;
  uint32_t now = millis();
  out["valid"] = valid;
  if (valid)
    out["utc"] = (uint32_t)utc;
  else
    out["utc"] = nullptr;
  out["timezone_minutes"] = timezone;
  out["state"] = !screenClockOnline ? "waiting_network"
                 : screenClockSynced && valid ? "synchronized"
                                             : "waiting_ntp";
  out["sntp_synced"] = screenClockSynced;
  if (screenClockSynced)
    out["sync_age_ms"] = (uint32_t)(now - screenClockSyncAt);
  else
    out["sync_age_ms"] = nullptr;
  if (screenClockMcuUtc) {
    out["mcu_snapshot_utc"] = screenClockMcuUtc;
    uint32_t age = now - screenClockMcuAt;
    out["mcu_snapshot_age_ms"] = age;
    if (valid && age < 60000)
      out["mcu_offset_seconds"] =
          (int64_t)screenClockMcuUtc + age / 1000 - (int64_t)utc;
    else
      out["mcu_offset_seconds"] = nullptr;
  } else {
    out["mcu_snapshot_utc"] = nullptr;
    out["mcu_snapshot_age_ms"] = nullptr;
    out["mcu_offset_seconds"] = nullptr;
  }
}

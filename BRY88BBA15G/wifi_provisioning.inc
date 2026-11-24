enum class G3WifiSetup { Idle, Waiting, Queued, Trial, Handoff, Rollback };
static G3WifiSetup g3WifiSetup = G3WifiSetup::Idle;
static uint32_t g3WifiDeadline = 0;
static bool g3SawProvisionRequest = false;
static uint32_t g3LastProvisionRequest = 0;
static char g3TrialSsid[33] = {}, g3TrialPassword[64] = {};
static const char *g3WifiTrialStatus = "idle";
static unsigned g3WifiTrialId = 0;

static bool g3WifiTrialActive() {
  return g3WifiSetup == G3WifiSetup::Queued || g3WifiSetup == G3WifiSetup::Trial;
}

static void startG3WifiProvisioning(uint32_t now) {
  // The stock MCU repeats the same UART frame until its own timeout.
  const bool repeated = g3SawProvisionRequest && uint32_t(now - g3LastProvisionRequest) < 5000;
  g3SawProvisionRequest = true;
  g3LastProvisionRequest = now;
  if (repeated || g3WifiSetup != G3WifiSetup::Idle || g3WifiWorkBusy())
    return;
  startAccessPoint();
  if (!g3AccessPointConfigured)
    return;
  g3WifiSetup = G3WifiSetup::Waiting;
  g3WifiDeadline = now + 120000;
  g3WifiTrialStatus = "idle";
  // Leave the old credentials intact and keep only the AP active while waiting.
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP);
  requestBusinessStatusRefresh();
}

static bool queueG3WifiTrial(const char *ssid, const char *password) {
  if (g3WifiTrialActive() || g3WifiSetup == G3WifiSetup::Handoff ||
      g3WifiSetup == G3WifiSetup::Rollback || g3WifiWorkBusy())
    return false;
  strlcpy(g3TrialSsid, ssid, sizeof(g3TrialSsid));
  strlcpy(g3TrialPassword, password, sizeof(g3TrialPassword));
  g3WifiSetup = G3WifiSetup::Queued;
  if (!++g3WifiTrialId)
    ++g3WifiTrialId;
  g3WifiTrialStatus = "pending";
  requestBusinessStatusRefresh();
  return true;
}

static bool saveG3WifiTrial() {
  DynamicJsonDocument candidate(std::min(configCapacity, config.memoryUsage() + 1024));
  if (!candidate.set(config))
    return false;
  candidate["wifi"]["ssid"] = String(g3TrialSsid);
  candidate["wifi"]["password"] = String(g3TrialPassword);
  return saveConfig(candidate);
}

static void clearG3WifiTrial() {
  if (g3WifiSetup == G3WifiSetup::Trial)
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
  g3WifiSetup = G3WifiSetup::Idle;
  memset(g3TrialPassword, 0, sizeof(g3TrialPassword));
}

static void closeG3SetupAccessPoint() {
  if (!g3NeedsProvisioning() && (WiFi.getMode() & WIFI_AP)) {
    if (WiFi.mode(WIFI_STA))
      g3AccessPointConfigured = false;
  }
}

static void pollG3WifiProvisioning(uint32_t now) {
  if (g3WifiSetup == G3WifiSetup::Idle)
    return;
  const bool rollingBack = g3WifiSetup == G3WifiSetup::Rollback;
  if (g3WifiSetup == G3WifiSetup::Handoff || rollingBack) {
    if (due(now, g3WifiDeadline)) {
      g3WifiSetup = G3WifiSetup::Idle;
      closeG3SetupAccessPoint();
      if (rollingBack)
        startWifi();
      requestBusinessStatusRefresh();
    }
    return;
  }
  if (g3WifiSetup == G3WifiSetup::Queued) {
    if (WiFi.scanComplete() == WIFI_SCAN_RUNNING)
      return;
    // Test credentials in driver RAM so a reboot still loads the saved network.
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) == ESP_OK) {
      WiFi.disconnect(false, false);
      g3StationGotIp.store(false);
      g3WifiSetup = G3WifiSetup::Trial;
      g3WifiDeadline = now + 30000;
      WiFi.begin(g3TrialSsid, g3TrialPassword);
      return;
    }
  }

  wifi_ap_record_t ap;
  const bool verified = g3WifiSetup == G3WifiSetup::Trial &&
      g3StationGotIp.load() && WiFi.status() == WL_CONNECTED &&
      WiFi.localIP() != IPAddress(0, 0, 0, 0) &&
      esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
      !strncmp(reinterpret_cast<const char *>(ap.ssid), g3TrialSsid, sizeof(ap.ssid));
  if (!verified && g3WifiSetup != G3WifiSetup::Queued && !due(now, g3WifiDeadline))
    return;

  const bool hadTrial = g3WifiTrialActive();
  const bool saved = verified && saveG3WifiTrial();
  const bool storageFailed = verified || g3WifiSetup == G3WifiSetup::Queued;
  g3WifiTrialStatus = saved ? "success" : storageFailed ? "storage_failure" : "timeout_rollback";
  clearG3WifiTrial();
  if (saved && (WiFi.getMode() & WIFI_AP)) {
    // Give the setup page a brief window to receive the new station address.
    g3WifiSetup = G3WifiSetup::Handoff;
    g3WifiDeadline = millis() + 4000;
  } else if (!saved) {
    WiFi.disconnect(false, false);
    if (hadTrial && !g3NeedsProvisioning() && (WiFi.getMode() & WIFI_AP)) {
      // Deliver the failed result over the hotspot before restoring the old link.
      g3WifiSetup = G3WifiSetup::Rollback;
      g3WifiDeadline = millis() + 1500;
    } else {
      closeG3SetupAccessPoint();
      startWifi();
    }
  }
  requestBusinessStatusRefresh();
}

/* Product adapter defines WIFI_PREFIX, WIFI_CONFIG, WIFI_STATE, WIFI_LOCK,
 * WIFI_GET, WIFI_SAVE. Candidate credentials never enter persistent RAM. */
#define WIFI_CAT_(a,b) a##b
#define WIFI_CAT(a,b) WIFI_CAT_(a,b)
#define WIFI_FN(name) WIFI_CAT(WIFI_PREFIX,name)
static struct { char ssid[33], password[65]; bool started, dhcp; uint32_t since; } wifi_candidate;

bool WIFI_FN(_wifi_trial)(const WIFI_CONFIG *candidate)
{
    size_t n = strlen(candidate->wifi_password);
    if (!candidate->ssid[0] || (n && (n < 8 || n > 63))) return false;
    mico_rtos_lock_mutex(&WIFI_LOCK);
    bool ok = !WIFI_STATE.wifi_trial_active && !WIFI_STATE.ota_active && !WIFI_STATE.reboot_pending;
    if (ok) {
        strcpy(wifi_candidate.ssid, candidate->ssid);
        strcpy(wifi_candidate.password, candidate->wifi_password);
        wifi_candidate.started = false; wifi_candidate.dhcp = false;
        WIFI_STATE.wifi_trial_active = true;
        strcpy(WIFI_STATE.wifi_trial_status, "pending");
    }
    mico_rtos_unlock_mutex(&WIFI_LOCK);
    return ok;
}

static void wifi_trial_dhcp(IPStatusTypedef *ip, void *arg)
{
    (void)arg;
    mico_rtos_lock_mutex(&WIFI_LOCK);
    if (WIFI_STATE.wifi_trial_active && wifi_candidate.started && ip->ip[0] && strcmp(ip->ip,"0.0.0.0"))
        wifi_candidate.dhcp = true;
    mico_rtos_unlock_mutex(&WIFI_LOCK);
}

static bool wifi_trial_prepare(WIFI_CONFIG *config, uint32_t now)
{
    mico_rtos_lock_mutex(&WIFI_LOCK);
    bool active = WIFI_STATE.wifi_trial_active;
    bool start = active && !wifi_candidate.started;
    if (active) {
        strcpy(config->ssid, wifi_candidate.ssid);
        strcpy(config->wifi_password, wifi_candidate.password);
    }
    if (start) { wifi_candidate.started = true; wifi_candidate.since = now; }
    mico_rtos_unlock_mutex(&WIFI_LOCK);
    return start;
}

/* Called only by the network worker. Caller reconnects the saved configuration
 * on failure. Merge with the latest config, preserving concurrent name/control saves. */
static int wifi_trial_finish(bool ready, uint32_t now)
{
    mico_rtos_lock_mutex(&WIFI_LOCK);
    bool active = WIFI_STATE.wifi_trial_active && wifi_candidate.started;
    uint32_t elapsed = now - wifi_candidate.since;
    ready = ready && wifi_candidate.dhcp;
    mico_rtos_unlock_mutex(&WIFI_LOCK);
    if (!active || (!ready && elapsed < 30000u)) return 0;
    bool saved = false;
    if (ready && elapsed < 30000u) {
        WIFI_CONFIG latest;
        WIFI_GET(&latest);
        strcpy(latest.ssid, wifi_candidate.ssid);
        strcpy(latest.wifi_password, wifi_candidate.password);
        saved = WIFI_SAVE(&latest);
    }
    mico_rtos_lock_mutex(&WIFI_LOCK);
    strcpy(WIFI_STATE.wifi_trial_status, saved ? "success" : ready && elapsed < 30000u ? "storage_failure" : "timeout_rollback");
    WIFI_STATE.wifi_trial_active = false;
    memset(&wifi_candidate, 0, sizeof(wifi_candidate));
    mico_rtos_unlock_mutex(&WIFI_LOCK);
    return saved ? 1 : -1;
}
#undef WIFI_FN
#undef WIFI_CAT
#undef WIFI_CAT_

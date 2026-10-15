#include "a1.h"
#include "config_legacy.h"
#include <stddef.h>

mico_mutex_t a1_lock, a1_config_lock;
a1_config_t *a1_config;
char a1_id[13];
a1_state_t a1_state;
mico_Context_t *a1_context;
static uint32_t save_at;
typedef struct { a1_config_t base; feature_config_t features; } app_storage_t;


void appRestoreDefault_callback(void *data, uint32_t size)
{
    if (size < sizeof(a1_config_t)) return;
    a1_config_t *c = data;
    memset(c, 0, sizeof(*c));
    if (size >= sizeof(app_storage_t)) feature_defaults(&((app_storage_t *)data)->features);
    c->magic = A1_CONFIG_MAGIC; c->version = A1_CONFIG_VERSION;
    c->mqtt_port = 1883; c->speed = 60;
    c->led_enabled = 1; c->brightness = 255; c->blue = 255;
}

void a1_config_get(a1_config_t *out)
{
    mico_rtos_lock_mutex(&a1_config_lock);
    *out = *a1_config;
    mico_rtos_unlock_mutex(&a1_config_lock);
}

bool a1_reset(bool full)
{
    app_storage_t *old = malloc(sizeof(*old));
    if (!old) return false;
    mico_rtos_lock_mutex(&a1_config_lock);
    mico_rtos_lock_mutex(&a1_lock);
    bool ok = !a1_state.ota_active && !a1_state.reboot_pending && !a1_state.wifi_trial_active;
    if (ok) { a1_state.reboot_pending = true; a1_state.reboot_at = mico_rtos_get_time() + 0x7fffffffu; }
    mico_rtos_unlock_mutex(&a1_lock);
    if (ok) {
        memcpy(old, a1_config, sizeof(*old));
        if (full) appRestoreDefault_callback(a1_config, sizeof(*old));
        else { a1_config->ssid[0] = 0; a1_config->wifi_password[0] = 0; a1_config->web_password[0] = 0; }
        ok = mico_system_context_update(a1_context) == kNoErr;
        if (!ok) memcpy(a1_config, old, sizeof(*old));
        mico_rtos_lock_mutex(&a1_lock);
        a1_state.reboot_pending = ok;
        if (ok) a1_state.reboot_at = mico_rtos_get_time() + 1000u;
        mico_rtos_unlock_mutex(&a1_lock);
    }
    mico_rtos_unlock_mutex(&a1_config_lock);
    free(old);
    return ok;
}

void a1_state_get(a1_state_t *out)
{
    mico_rtos_lock_mutex(&a1_lock);
    *out = a1_state;
    mico_rtos_unlock_mutex(&a1_lock);
}

void a1_control_changed(void)
{
    a1_config->revision++;
    a1_state.config_generation++;
    a1_state.config_dirty = true;
    save_at = mico_rtos_get_time() + A1_SAVE_DELAY_MS;
    a1_state.led_until = mico_rtos_get_time() + 2000;
}

/* Configuration mutex serializes flash updates with OTA/reboot reservation. */
static bool flush_locked(void)
{
    OSStatus err = mico_system_context_update(a1_context);
    mico_rtos_lock_mutex(&a1_lock);
    if (err == kNoErr) { a1_state.config_dirty = false; a1_state.save_error[0] = 0; }
    else {
        snprintf(a1_state.save_error, sizeof(a1_state.save_error), "Config flash write failed: %ld", (long)err);
        save_at = mico_rtos_get_time() + A1_SAVE_DELAY_MS;
    }
    mico_rtos_unlock_mutex(&a1_lock);
    return err == kNoErr;
}

bool a1_config_save(const a1_config_t *c)
{
    mico_rtos_lock_mutex(&a1_config_lock);
    mico_rtos_lock_mutex(&a1_lock);
    bool reset = a1_config->magic != A1_CONFIG_MAGIC || a1_config->version != A1_CONFIG_VERSION;
    bool valid = !a1_state.ota_active && !a1_state.reboot_pending &&
        c->magic == A1_CONFIG_MAGIC && c->version == A1_CONFIG_VERSION &&
        (reset || c->revision == a1_config->revision);
    mico_rtos_unlock_mutex(&a1_lock);
    if (!valid) { mico_rtos_unlock_mutex(&a1_config_lock); return false; }
    a1_config_t old = *a1_config;
    *a1_config = *c; a1_config->revision = old.revision + 1;
    bool ok = flush_locked();
    if (!ok) *a1_config = old;
    else {
        mico_rtos_lock_mutex(&a1_lock); a1_state.config_generation++; mico_rtos_unlock_mutex(&a1_lock);
    }
    mico_rtos_unlock_mutex(&a1_config_lock);
    return ok;
}

bool a1_config_flush(bool force)
{
    mico_rtos_lock_mutex(&a1_config_lock);
    mico_rtos_lock_mutex(&a1_lock);
    bool busy = a1_state.ota_active || a1_state.reboot_pending;
    bool due = a1_state.config_dirty && (force || (int32_t)(mico_rtos_get_time() - save_at) >= 0);
    mico_rtos_unlock_mutex(&a1_lock);
    bool ok = !busy && (!due || flush_locked());
    mico_rtos_unlock_mutex(&a1_config_lock);
    return ok;
}

static bool exclusive_begin(bool ota, uint32_t delay)
{
    mico_rtos_lock_mutex(&a1_config_lock);
    mico_rtos_lock_mutex(&a1_lock);
    bool free = !a1_state.ota_active && !a1_state.reboot_pending && !a1_state.wifi_trial_active;
    bool dirty = a1_state.config_dirty;
    if (free) {
        a1_state.ota_active = ota; a1_state.reboot_pending = !ota;
        a1_state.reboot_at = mico_rtos_get_time() + 0x7fffffffu;
    }
    mico_rtos_unlock_mutex(&a1_lock);
    if (free && dirty && !flush_locked()) {
        mico_rtos_lock_mutex(&a1_lock);
        a1_state.ota_active = a1_state.reboot_pending = false;
        mico_rtos_unlock_mutex(&a1_lock); free = false;
    }
    if (free && !ota) {
        mico_rtos_lock_mutex(&a1_lock);
        a1_state.reboot_at = mico_rtos_get_time() + delay;
        mico_rtos_unlock_mutex(&a1_lock);
    }
    mico_rtos_unlock_mutex(&a1_config_lock);
    return free;
}

bool a1_reboot_later(uint32_t ms) { return exclusive_begin(false, ms); }
bool a1_upgrade_reserve(void) { return exclusive_begin(true, 0); }

static OSStatus startup_result(const char *stage, OSStatus err)
{
    custom_log("A1", "Startup %s: %ld", stage, (long)err);
    return err;
}

int application_start(void)
{
    custom_log("A1", "Starting %s", A1_VERSION);
    OSStatus err = mico_rtos_init_mutex(&a1_lock);
    if (err != kNoErr) return startup_result("state mutex", err);
    err = mico_rtos_init_mutex(&a1_config_lock);
    if (err != kNoErr) return startup_result("config mutex", err);
    const mico_logic_partition_t *parameter = MicoFlashGetInfo(MICO_PARTITION_PARAMETER_1);
    const mico_logic_partition_t *backup = MicoFlashGetInfo(MICO_PARTITION_PARAMETER_2);
    size_t required = sizeof(system_config_t) + sizeof(app_storage_t) + 2;
    if (!parameter || !backup || parameter->partition_length < required || backup->partition_length < required) return startup_result("config allocation/layout", kNoMemoryErr);
#ifdef MICO_EXTRA_AP_NUM
    if (sizeof(system_config_t) + sizeof(app_storage_t) > PARA_SYS_EXTRA_OFFSET) return startup_result("config allocation/layout", kNoMemoryErr);
#endif
    a1_context = mico_system_context_init(sizeof(app_storage_t));
    if (!a1_context) return startup_result("config allocation/layout", kNoMemoryErr);
    a1_config = mico_system_context_get_user_data(a1_context);
    if (!a1_config) return startup_result("config allocation/layout", kNoMemoryErr);
    a1_config_t c;
    /* The SDK validates the entire partition before loading these bytes. */
    if (a1_config->magic == A1_CONFIG_MAGIC &&
        (a1_config->version == 1 || a1_config->version == 2)) {
        a1_storage_v2_t *old = malloc(sizeof(*old));
        if (!old) return startup_result("config allocation/layout", kNoMemoryErr);
        memcpy(old, a1_config, sizeof(*old));
        memset(a1_config, 0, sizeof(app_storage_t));
        a1_migrate_v2(a1_config, &((app_storage_t *)a1_config)->features, old);
        free(old);
        if (mico_system_context_update(a1_context) != kNoErr) return startup_result("config persist", kUnknownErr);
    }
    a1_config_get(&c);
    if (c.magic != A1_CONFIG_MAGIC || c.version != A1_CONFIG_VERSION) {
        appRestoreDefault_callback(&c, sizeof(c));
        if (!a1_config_save(&c)) return startup_result("config persist", kUnknownErr);
    }
    feature_config_t *extension = &((app_storage_t *)a1_config)->features;
    if (extension->magic != FEATURE_MAGIC) {
        feature_defaults(extension);
        if (mico_system_context_update(a1_context) != kNoErr) return startup_result("config persist", kUnknownErr);
    }
    err = startup_result("MiCO system", mico_system_init(a1_context));
    if (err != kNoErr) return err;
    uint8_t mac[6]; mico_wlan_get_mac_address(mac);
    snprintf(a1_id, sizeof(a1_id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    a1_config_get(&c);
    if (!c.name[0]) {
        snprintf(c.name, sizeof(c.name), "phicomm_a1_%s", a1_id + 6);
        if (!a1_config_save(&c)) return startup_result("config persist", kUnknownErr);
    }
    snprintf(a1_state.ap_name, sizeof(a1_state.ap_name), "A1-AP-%s", a1_id + 6);
    for (char *p = a1_state.ap_name; *p; ++p)
        if (*p >= 'a' && *p <= 'f') *p -= 'a' - 'A';
    strcpy(a1_state.ota_state, "idle");
    a1_state.led_until = mico_rtos_get_time() + 2000;
    err = startup_result("features", feature_start()); if (err != kNoErr) return err;
    err = startup_result("network", a1_network_start()); if (err != kNoErr) return err;
    err = startup_result("HTTP", a1_http_start()); if (err != kNoErr) return err;
    /* Keep provisioning available when peripheral initialization fails. */
    err = startup_result("hardware", a1_hardware_start());
    if (err != kNoErr) custom_log("A1", "Hardware unavailable; provisioning remains active");
    err = startup_result("MQTT", a1_mqtt_start());
    if (err != kNoErr) custom_log("A1", "MQTT unavailable; provisioning remains active");
    custom_log("A1", "Started %s, id %s", A1_VERSION, a1_id);
    return kNoErr;
}

void feature_config_get(feature_config_t *out)
{
    mico_rtos_lock_mutex(&a1_config_lock);
    *out = ((app_storage_t *)a1_config)->features;
    mico_rtos_unlock_mutex(&a1_config_lock);
}
bool feature_config_save(const feature_config_t *c)
{
    feature_config_t *old = malloc(sizeof(*old));
    if (!old) return false;
    mico_rtos_lock_mutex(&a1_config_lock);
    feature_config_t *live = &((app_storage_t *)a1_config)->features;
    mico_rtos_lock_mutex(&a1_lock);
    bool busy = a1_state.ota_active || a1_state.reboot_pending;
    mico_rtos_unlock_mutex(&a1_lock);
    bool ok = !busy && c->magic == FEATURE_MAGIC && live->revision == c->revision;
    if (ok) {
        *old = *live; *live = *c; live->revision++;
        ok = mico_system_context_update(a1_context) == kNoErr;
        if (!ok) *live = *old;
    }
    mico_rtos_unlock_mutex(&a1_config_lock);
    free(old); return ok;
}

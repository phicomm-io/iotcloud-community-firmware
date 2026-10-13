#include "a1.h"

mico_mutex_t a1_lock, a1_config_lock;
a1_config_t *a1_config;
char a1_id[13];
a1_state_t a1_state;
mico_Context_t *a1_context;
static uint32_t save_at;

void appRestoreDefault_callback(void *data, uint32_t size)
{
    if (size < sizeof(a1_config_t)) return;
    a1_config_t *c = data;
    memset(c, 0, sizeof(*c));
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
    bool free = !a1_state.ota_active && !a1_state.reboot_pending;
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

int application_start(void)
{
    OSStatus err = mico_rtos_init_mutex(&a1_lock);
    if (err != kNoErr) return err;
    err = mico_rtos_init_mutex(&a1_config_lock);
    if (err != kNoErr) return err;
    a1_context = mico_system_context_init(sizeof(a1_config_t));
    if (!a1_context) return kNoMemoryErr;
    a1_config = mico_system_context_get_user_data(a1_context);
    if (!a1_config) return kNoMemoryErr;
    a1_config_t c; a1_config_get(&c);
    if (c.magic != A1_CONFIG_MAGIC || c.version != A1_CONFIG_VERSION) {
        appRestoreDefault_callback(&c, sizeof(c));
        if (!a1_config_save(&c)) return kUnknownErr;
    }
    err = mico_system_init(a1_context);
    if (err != kNoErr) return err;
    uint8_t mac[6]; mico_wlan_get_mac_address(mac);
    snprintf(a1_id, sizeof(a1_id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    a1_config_get(&c);
    if (!c.name[0]) {
        snprintf(c.name, sizeof(c.name), "phicomm_a1_%s", a1_id + 6);
        if (!a1_config_save(&c)) return kUnknownErr;
    }
    snprintf(a1_state.ap_name, sizeof(a1_state.ap_name), "A1-AP-%s", a1_id + 6);
    strcpy(a1_state.ota_state, "idle");
    a1_state.led_until = mico_rtos_get_time() + 2000;
    err = a1_hardware_start(); if (err != kNoErr) return err;
    err = a1_network_start(); if (err != kNoErr) return err;
    err = a1_mqtt_start(); if (err != kNoErr) return err;
    err = a1_http_start(); if (err != kNoErr) return err;
    custom_log("A1", "Started %s, id %s", A1_VERSION, a1_id);
    return kNoErr;
}

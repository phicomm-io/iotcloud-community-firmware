#include "m1.h"

mico_mutex_t m1_lock;
static mico_mutex_t config_lock;
static m1_config_t *app_config;
char m1_id[13];
m1_state_t m1_state;
mico_Context_t *m1_context;

void appRestoreDefault_callback(void *data, uint32_t size)
{
    if (size < sizeof(m1_config_t)) return;
    m1_config_t *c = data;
    memset(c, 0, sizeof(*c));
    c->magic = M1_CONFIG_MAGIC; c->version = M1_CONFIG_VERSION;
    strcpy(c->name, "Phicomm M1"); c->mqtt_port = 1883;
    c->brightness = c->last_on = 4; c->timezone_minutes = 480;
}

void m1_config_get(m1_config_t *out)
{
    mico_rtos_lock_mutex(&config_lock);
    memcpy(out, app_config, sizeof(*out));
    mico_rtos_unlock_mutex(&config_lock);
}

bool m1_config_save(const m1_config_t *c)
{
    mico_rtos_lock_mutex(&config_lock);
    if (c->revision != app_config->revision && c->magic == app_config->magic) {
        mico_rtos_unlock_mutex(&config_lock); return false;
    }
    m1_config_t old = *app_config;
    memcpy(app_config, c, sizeof(*c));
    app_config->revision = old.revision + 1;
    OSStatus err = mico_system_context_update(m1_context);
    if (err != kNoErr) memcpy(app_config, &old, sizeof(old));
    mico_rtos_unlock_mutex(&config_lock);
    if (err != kNoErr) return false;
    mico_rtos_lock_mutex(&m1_lock);
    m1_state.config_generation++;
    mico_rtos_unlock_mutex(&m1_lock);
    return true;
}

void m1_state_get(m1_state_t *out)
{
    mico_rtos_lock_mutex(&m1_lock);
    memcpy(out, &m1_state, sizeof(*out));
    mico_rtos_unlock_mutex(&m1_lock);
}

void m1_reboot_later(uint32_t ms)
{
    mico_rtos_lock_mutex(&m1_lock);
    m1_state.reboot_pending = true;
    m1_state.reboot_at = mico_rtos_get_time() + ms;
    mico_rtos_unlock_mutex(&m1_lock);
}

int application_start(void)
{
    OSStatus err = mico_rtos_init_mutex(&m1_lock);
    if (err != kNoErr) return err;
    err = mico_rtos_init_mutex(&config_lock);
    if (err != kNoErr) return err;
    m1_context = mico_system_context_init(sizeof(m1_config_t));
    if (!m1_context) return kNoMemoryErr;
    app_config = mico_system_context_get_user_data(m1_context);
    if (!app_config) return kNoMemoryErr;
    m1_config_t c;
    m1_config_get(&c);
    if (c.magic != M1_CONFIG_MAGIC || c.version != M1_CONFIG_VERSION) {
        /* Reset only application data: never restore the whole device context. */
        appRestoreDefault_callback(&c, sizeof(c));
        if (!m1_config_save(&c)) return kUnknownErr;
    }
    err = mico_system_init(m1_context);
    if (err != kNoErr) return err;
    uint8_t mac[6];
    mico_wlan_get_mac_address(mac);
    snprintf(m1_id, sizeof(m1_id), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(m1_state.ap_name, sizeof(m1_state.ap_name), "M1-AP-%s", m1_id + 6);
    strcpy(m1_state.ota_state, "idle");
    m1_state.brightness_requested = c.brightness;
    err = m1_uart_start(); if (err != kNoErr) return err;
    err = m1_network_start(); if (err != kNoErr) return err;
    err = m1_mqtt_start(); if (err != kNoErr) return err;
    err = m1_http_start(); if (err != kNoErr) return err;
    custom_log("M1", "Started %s, id %s", M1_VERSION, m1_id);
    return kNoErr;
}

#include "../common/automation.h"
#include "../common/service.h"
#include "m1.h"
#include "config_legacy.h"
#include <stddef.h>

mico_mutex_t m1_lock;
static mico_mutex_t config_lock;
static m1_config_t *app_config;
typedef struct { m1_config_t base; feature_config_t features; automation_config_t automation; } app_storage_t;

char m1_id[13];
m1_state_t m1_state;
mico_Context_t *m1_context;

void appRestoreDefault_callback(void *data, uint32_t size)
{
    if (size < sizeof(m1_config_t)) return;
    m1_config_t *c = data;
    memset(c, 0, sizeof(*c));
    if (size >= sizeof(app_storage_t)) feature_defaults(&((app_storage_t *)data)->features);
    if (size >= sizeof(app_storage_t)) automation_defaults(&((app_storage_t *)data)->automation);
    c->magic = M1_CONFIG_MAGIC; c->version = M1_CONFIG_VERSION;
    c->mqtt_port = 1883; c->mqtt_topic_mode = MQTT_TOPIC_TMCP;
    c->brightness = c->last_on = 4; c->timezone_minutes = 480;
}

void m1_config_get(m1_config_t *out)
{
    mico_rtos_lock_mutex(&config_lock);
    memcpy(out, app_config, sizeof(*out));
    mico_rtos_unlock_mutex(&config_lock);
}

void m1_ntp_host_get(char out[64])
{
    mico_rtos_lock_mutex(&config_lock);
    memcpy(out, ((app_storage_t *)app_config)->features.ntp_host, 64);
    out[63] = 0;
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
    feature_config_t *features = &((app_storage_t *)app_config)->features;
    int16_t old_timezone = features->timezone_minutes;
    features->timezone_minutes = c->timezone_minutes;
    OSStatus err = mico_system_context_update(m1_context);
    if (err != kNoErr) { memcpy(app_config, &old, sizeof(old)); features->timezone_minutes = old_timezone; }
    mico_rtos_unlock_mutex(&config_lock);
    if (err != kNoErr) return false;
    mico_rtos_lock_mutex(&m1_lock);
    m1_state.config_generation++;
    mico_rtos_unlock_mutex(&m1_lock);
    return true;
}

bool m1_reset(bool full)
{
    app_storage_t *old = malloc(sizeof(*old));
    if (!old) return false;
    mico_rtos_lock_mutex(&config_lock);
    mico_rtos_lock_mutex(&m1_lock);
    bool ok = !m1_state.ota_active && !m1_state.reboot_pending && !m1_state.wifi_trial_active;
    if (ok) { m1_state.reboot_pending = true; m1_state.reboot_at = mico_rtos_get_time() + 0x7fffffffu; }
    mico_rtos_unlock_mutex(&m1_lock);
    if (ok) {
        memcpy(old, app_config, sizeof(*old));
        if (full) appRestoreDefault_callback(app_config, sizeof(*old));
        else { app_config->ssid[0] = 0; app_config->wifi_password[0] = 0; app_config->web_password[0] = 0; }
        ok = mico_system_context_update(m1_context) == kNoErr;
        if (!ok) memcpy(app_config, old, sizeof(*old));
        mico_rtos_lock_mutex(&m1_lock);
        m1_state.reboot_pending = ok;
        if (ok) m1_state.reboot_at = mico_rtos_get_time() + 1000u;
        mico_rtos_unlock_mutex(&m1_lock);
    }
    mico_rtos_unlock_mutex(&config_lock);
    free(old);
    return ok;
}

void m1_state_get(m1_state_t *out)
{
    mico_rtos_lock_mutex(&m1_lock);
    memcpy(out, &m1_state, sizeof(*out));
    mico_rtos_unlock_mutex(&m1_lock);
}

bool m1_reboot_later(uint32_t ms)
{
    mico_rtos_lock_mutex(&m1_lock);
    bool ok = !m1_state.ota_active && !m1_state.reboot_pending && !m1_state.wifi_trial_active;
    if (ok) { m1_state.reboot_pending = true; m1_state.reboot_at = mico_rtos_get_time() + ms; }
    mico_rtos_unlock_mutex(&m1_lock);
    return ok;
}

int application_start(void)
{
    service_init();
    OSStatus err = mico_rtos_init_mutex(&m1_lock);
    if (err != kNoErr) return err;
    err = mico_rtos_init_mutex(&config_lock);
    if (err != kNoErr) return err;
    const mico_logic_partition_t *parameter = MicoFlashGetInfo(MICO_PARTITION_PARAMETER_1);
    const mico_logic_partition_t *backup = MicoFlashGetInfo(MICO_PARTITION_PARAMETER_2);
    size_t required = sizeof(system_config_t) + sizeof(app_storage_t) + 2;
    if (!parameter || !backup || parameter->partition_length < required || backup->partition_length < required) return kNoMemoryErr;
#ifdef MICO_EXTRA_AP_NUM
    if (sizeof(system_config_t) + sizeof(app_storage_t) > PARA_SYS_EXTRA_OFFSET) return kNoMemoryErr;
#endif
    m1_context = mico_system_context_init(sizeof(app_storage_t));
    if (!m1_context) return kNoMemoryErr;
    app_config = mico_system_context_get_user_data(m1_context);
    if (!app_config) return kNoMemoryErr;
    m1_config_t c;
    /* The SDK validates the entire partition before loading these bytes. */
    if (app_config->magic != M1_CONFIG_MAGIC || app_config->version < 1 || app_config->version > M1_CONFIG_VERSION) {
        service_event("error","config","unsupported_layout");return kUnknownErr;
    }
    if (app_config->magic == M1_CONFIG_MAGIC &&
        (app_config->version == 1 || app_config->version == 2)) {
        m1_storage_v2_t *old = malloc(sizeof(*old));
        if (!old) return kNoMemoryErr;
        memcpy(old, app_config, sizeof(*old));
        memset(app_config, 0, sizeof(app_storage_t));
        m1_migrate_v2(app_config, &((app_storage_t *)app_config)->features, old);
        free(old);
        m1_migrate_response_permissions(app_config, &((app_storage_t *)app_config)->features);
        if (mico_system_context_update(m1_context) != kNoErr) return kUnknownErr;
    }
    if (app_config->magic == M1_CONFIG_MAGIC && app_config->version == 3) {
        m1_migrate_response_permissions(app_config, &((app_storage_t *)app_config)->features);
        if (mico_system_context_update(m1_context) != kNoErr) return kUnknownErr;
    }
    if (app_config->magic == M1_CONFIG_MAGIC && app_config->version == 4) {
        ((app_storage_t *)app_config)->automation.magic=0;
        app_config->version = M1_CONFIG_VERSION;
    }
    if (((app_storage_t *)app_config)->automation.magic != AUTOMATION_MAGIC) {
        automation_config_t *next=&((app_storage_t *)app_config)->automation;
        automation_defaults(next);
        feature_config_t *f=&((app_storage_t *)app_config)->features;
        for(unsigned i=0;i<f->event_count && i<8;i++) {
            automation_task_t *t=&next->tasks[i];t->id=next->next_id++;t->days=f->events[i].days;t->minute=f->events[i].minute;
            t->action.kind=AUTO_FAN;t->action.value=f->events[i].on;t->action.speed=f->events[i].speed;
            t->enabled=0;t->state=AUTO_MIGRATED_DISABLED;
        }
        f->event_count=0;f->scheduled_off=f->scheduled_valid=0;
        if (mico_system_context_update(m1_context) != kNoErr) return kUnknownErr;
    }
    /* Normalize the retained ABI byte without changing credentials or layout. */
    if (app_config->mqtt_topic_mode != MQTT_TOPIC_TMCP) {
        app_config->mqtt_topic_mode = MQTT_TOPIC_TMCP;
        if (mico_system_context_update(m1_context) != kNoErr) return kUnknownErr;
    }
    m1_config_get(&c);
    if (c.magic != M1_CONFIG_MAGIC || c.version != M1_CONFIG_VERSION) {
        service_event("error","config","unsupported_layout");
        return kUnknownErr;
    }
    feature_config_t *extension = &((app_storage_t *)app_config)->features;
    if (extension->magic != FEATURE_MAGIC) {
        feature_defaults(extension);
        if (mico_system_context_update(m1_context) != kNoErr) return kUnknownErr;
    }
    err = mico_system_init(m1_context);
    if (err != kNoErr) return err;
    uint8_t mac[6];
    mico_wlan_get_mac_address(mac);
    snprintf(m1_id, sizeof(m1_id), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (!c.name[0]) {
        snprintf(c.name, sizeof(c.name), "M1-%02X%02X", mac[4], mac[5]);
        if (!m1_config_save(&c)) return kUnknownErr;
    }
    snprintf(m1_state.ap_name, sizeof(m1_state.ap_name), "M1-AP-%s", m1_id + 6);
    for (char *p = m1_state.ap_name; *p; ++p)
        if (*p >= 'a' && *p <= 'f') *p -= 'a' - 'A';
    strcpy(m1_state.ota_state, "idle");
    m1_state.brightness_requested = c.brightness;
    err = feature_start(); if (err != kNoErr) return err;
    err = m1_uart_start(); if (err != kNoErr) return err;
    err = m1_network_start(); if (err != kNoErr) return err;
    err = m1_mqtt_start(); if (err != kNoErr) return err;
    err = m1_http_start(); if (err != kNoErr) return err;
    custom_log("M1", "Started %s, id %s", M1_VERSION, m1_id);
    return kNoErr;
}

void feature_config_get(feature_config_t *out)
{
    mico_rtos_lock_mutex(&config_lock);
    *out = ((app_storage_t *)app_config)->features;
    mico_rtos_unlock_mutex(&config_lock);
}
bool feature_config_save(const feature_config_t *c)
{
    feature_config_t *old = malloc(sizeof(*old));
    if (!old) return false;
    mico_rtos_lock_mutex(&config_lock);
    feature_config_t *live = &((app_storage_t *)app_config)->features;
    mico_rtos_lock_mutex(&m1_lock);
    bool busy = m1_state.ota_active || m1_state.reboot_pending;
    mico_rtos_unlock_mutex(&m1_lock);
    bool ok = !busy && c->magic == FEATURE_MAGIC && live->revision == c->revision;
    if (ok) {
        int16_t previous_timezone = app_config->timezone_minutes;
        uint32_t previous_revision = app_config->revision;
        *old = *live; *live = *c; live->revision++;
        if (app_config->timezone_minutes != c->timezone_minutes) {
            app_config->timezone_minutes = c->timezone_minutes;
            app_config->revision++;
        }
        ok = mico_system_context_update(m1_context) == kNoErr;
        if (!ok) {
            *live = *old;
            app_config->timezone_minutes = previous_timezone;
            app_config->revision = previous_revision;
        }
    }
    mico_rtos_unlock_mutex(&config_lock);
    free(old); return ok;
}

void automation_load(automation_config_t *out) {
    mico_rtos_lock_mutex(&config_lock);*out=((app_storage_t *)app_config)->automation;out->timezone=((app_storage_t *)app_config)->features.timezone_minutes;mico_rtos_unlock_mutex(&config_lock);
}
bool automation_save(const automation_config_t *next) {
    automation_config_t *old=malloc(sizeof(*old));if(!old)return false;
    mico_rtos_lock_mutex(&config_lock);
    automation_config_t *live=&((app_storage_t *)app_config)->automation;
    mico_rtos_lock_mutex(&m1_lock);bool busy=m1_state.ota_active || m1_state.reboot_pending || m1_state.wifi_trial_active;mico_rtos_unlock_mutex(&m1_lock);
    bool ok=!busy && next->magic==AUTOMATION_MAGIC && next->revision==live->revision+1;
    if(ok){*old=*live;*live=*next;ok=mico_system_context_update(m1_context)==kNoErr;if(!ok)*live=*old;}
    mico_rtos_unlock_mutex(&config_lock);free(old);return ok;
}

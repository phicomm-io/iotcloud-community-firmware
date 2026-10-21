#define FEATURE_DEVICE_A1
#include "../common/automation.h"
#include "../common/service.h"
#include "../common/watchdog.h"
#include "a1.h"
#include "config_legacy.h"
#include "storage.h"
#include <stddef.h>

mico_mutex_t a1_lock, a1_config_lock;
a1_config_t *a1_config;
char a1_id[13];
a1_state_t a1_state;
mico_Context_t *a1_context;
static uint32_t save_at;
static a1_filter_clock_t filter_clock;

static a1_filter_storage_t *filter_storage(void)
{ return &((app_storage_t *)a1_config)->filter; }

void a1_filter_tick(bool running)
{
    mico_rtos_lock_mutex(&a1_config_lock);
    a1_filter_advance(filter_storage(), &filter_clock, mico_rtos_get_time(), running);
    mico_rtos_unlock_mutex(&a1_config_lock);
}

void a1_filter_get(a1_filter_storage_t *out)
{
    mico_rtos_lock_mutex(&a1_config_lock);
    a1_filter_advance(filter_storage(), &filter_clock, mico_rtos_get_time(), filter_clock.running);
    *out = *filter_storage();
    mico_rtos_unlock_mutex(&a1_config_lock);
}

static bool filter_pending(void)
{ return filter_storage()->used_seconds != filter_clock.saved_seconds; }


void appRestoreDefault_callback(void *data, uint32_t size)
{
    if (size < sizeof(a1_config_t)) return;
    a1_config_t *c = data;
    memset(c, 0, sizeof(*c));
    if (size >= sizeof(app_storage_t)) a1_storage_features_defaults((app_storage_t *)data);
    if (size >= sizeof(app_storage_t)) a1_storage_automation_defaults((app_storage_t *)data);
    if (size >= sizeof(app_storage_t)) a1_filter_defaults(&((app_storage_t *)data)->filter);
    if(size>=sizeof(app_storage_t))memset(&((app_storage_t *)data)->bemfa,0,sizeof(bemfa_config_t));
    c->magic = A1_CONFIG_MAGIC; c->version = A1_CONFIG_VERSION;
    c->mqtt_port = 1883; c->mqtt_topic_mode = MQTT_TOPIC_TMCP; c->speed = 60;
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
        a1_filter_advance(filter_storage(), &filter_clock, mico_rtos_get_time(), filter_clock.running);
        memcpy(old, a1_config, sizeof(*old));
        if (full) appRestoreDefault_callback(a1_config, sizeof(*old));
        else { a1_config->ssid[0] = 0; a1_config->wifi_password[0] = 0; a1_config->web_password[0] = 0; }
        ok = mico_system_context_update(a1_context) == kNoErr;
        if (!ok) memcpy(a1_config, old, sizeof(*old));
        else {
            filter_clock.saved_seconds = filter_storage()->used_seconds;
            if (full) filter_clock = (a1_filter_clock_t){0};
        }
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
    a1_filter_advance(filter_storage(), &filter_clock, mico_rtos_get_time(), filter_clock.running);
    OSStatus err = mico_system_context_update(a1_context);
    if (err == kNoErr) filter_clock.saved_seconds = filter_storage()->used_seconds;
    mico_rtos_lock_mutex(&a1_lock);
    if (err == kNoErr) { a1_state.config_dirty = false; a1_state.save_error[0] = 0; }
    else {
        a1_state.config_dirty = true;
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
    a1_filter_advance(filter_storage(), &filter_clock, mico_rtos_get_time(), filter_clock.running);
    bool checkpoint = filter_storage()->used_seconds - filter_clock.saved_seconds >= A1_FILTER_CHECKPOINT_SECONDS;
    bool due = a1_state.config_dirty ? (force || (int32_t)(mico_rtos_get_time() - save_at) >= 0) :
        (filter_pending() && (force || checkpoint || !filter_clock.running));
    mico_rtos_unlock_mutex(&a1_lock);
    bool ok = !busy && (!due || flush_locked());
    mico_rtos_unlock_mutex(&a1_config_lock);
    return ok;
}

const char *a1_filter_change(unsigned lifetime_hours, bool reset)
{
    mico_rtos_lock_mutex(&a1_config_lock);
    mico_rtos_lock_mutex(&a1_lock);
    bool busy = a1_state.ota_active || a1_state.reboot_pending || a1_state.wifi_trial_active;
    mico_rtos_unlock_mutex(&a1_lock);
    if (busy) { mico_rtos_unlock_mutex(&a1_config_lock); return "busy"; }
    a1_filter_advance(filter_storage(), &filter_clock, mico_rtos_get_time(), filter_clock.running);
    a1_filter_storage_t old = *filter_storage();
    if (reset) filter_storage()->used_seconds = 0;
    else filter_storage()->lifetime_hours = lifetime_hours;
    bool ok = flush_locked();
    if (!ok) *filter_storage() = old;
    else if (reset) filter_clock.remainder_ms = 0;
    mico_rtos_unlock_mutex(&a1_config_lock);
    return ok ? "accepted" : "storage_error";
}

static bool exclusive_begin(bool ota, uint32_t delay)
{
    mico_rtos_lock_mutex(&a1_config_lock);
    mico_rtos_lock_mutex(&a1_lock);
    bool free = !a1_state.ota_active && !a1_state.reboot_pending && !a1_state.wifi_trial_active;
    a1_filter_advance(filter_storage(), &filter_clock, mico_rtos_get_time(), filter_clock.running);
    bool dirty = a1_state.config_dirty || filter_pending();
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
    OSStatus watchdog_err = watchdog_start();
    if (watchdog_err != kNoErr) {
        custom_log("Watchdog", "Startup failed: %ld", (long)watchdog_err);
        return watchdog_err;
    }
    service_init();
    watchdog_boot_event();
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
    /* The SDK validates MiCO's partition, not the application's layout.
     * zA1's J-Link installer preserves parameters from the previous firmware. */
    if (a1_config->magic != A1_CONFIG_MAGIC) {
        memset(a1_config, 0, sizeof(app_storage_t));
        appRestoreDefault_callback(a1_config, sizeof(app_storage_t));
        if (mico_system_context_update(a1_context) != kNoErr) return startup_result("config persist", kUnknownErr);
        service_event("info", "config", "foreign_layout_reset");
    }
    if (a1_config->version < 1 || a1_config->version > A1_CONFIG_VERSION) {
        service_event("error","config","unsupported_layout");return kUnknownErr;
    }
    bool co2_migration = a1_config->version < 9;
    bool panel_migration = a1_config->version < 8;
    bool auto_migration = a1_config->version < 7;
    bool button_migration = a1_config->version < 6;
    if (button_migration) memset(((app_storage_t *)a1_config)->combination_buttons, 0,
                                 sizeof(((app_storage_t *)a1_config)->combination_buttons));
    bool filter_migration = a1_config->version < 5;
    if (filter_migration) a1_filter_defaults(filter_storage());
    if (a1_config->magic == A1_CONFIG_MAGIC &&
        (a1_config->version == 1 || a1_config->version == 2)) {
        a1_storage_v2_t *old = malloc(sizeof(*old));
        if (!old) return startup_result("config allocation/layout", kNoMemoryErr);
        memcpy(old, a1_config, sizeof(*old));
        memset(a1_config, 0, sizeof(app_storage_t));
        feature_config_t next; a1_migrate_v2(a1_config, &next, old);
        a1_storage_features_put((app_storage_t *)a1_config, &next);
        free(old);
        if (mico_system_context_update(a1_context) != kNoErr) return startup_result("config persist", kUnknownErr);
    }
    if (a1_config->magic == A1_CONFIG_MAGIC && a1_config->version == 3) {
        ((app_storage_t *)a1_config)->automation.magic=0;
    }
    if (a1_config->version == 3 || a1_config->version == 4 || a1_config->version == 5 || a1_config->version == 6 || a1_config->version == 7 || a1_config->version == 8) a1_config->version = A1_CONFIG_VERSION;
    if (filter_storage()->magic != A1_FILTER_MAGIC || !filter_storage()->lifetime_hours ||
        filter_storage()->lifetime_hours > A1_FILTER_MAX_HOURS) {
        a1_filter_defaults(filter_storage()); filter_migration = true;
    }
    filter_clock.saved_seconds = filter_storage()->used_seconds;
    if (auto_migration) {
        app_storage_t *s = (app_storage_t *)a1_config;
        memset(&s->automatic, 0, sizeof(s->automatic));
        s->automatic.fallback_speed = 30;
        for (unsigned i=0;i<4;i++) {
            s->automatic.night[i].min_speed = 0;
            s->automatic.night[i].fallback_speed = 30;
        }
        if (s->automation.magic == AUTOMATION_MAGIC && s->automation.buttons[0][1].kind == AUTO_NONE)
            s->automation.buttons[0][1].kind = AUTO_FAN_MODE_TOGGLE;
    }
    if (co2_migration) feature_co2_defaults(&((app_storage_t *)a1_config)->co2);
    if (panel_migration) a1_storage_panel_buttons_migrate((app_storage_t *)a1_config);
    if ((filter_migration || button_migration || auto_migration || panel_migration || co2_migration) && mico_system_context_update(a1_context) != kNoErr) return startup_result("filter persist", kUnknownErr);
    if (((app_storage_t *)a1_config)->automation.magic != AUTOMATION_MAGIC) {
        automation_config_t *next=malloc(sizeof(*next));
        if (!next) return startup_result("automation allocation", kNoMemoryErr);
        automation_defaults(next);
        feature_config_storage_t *f=&((app_storage_t *)a1_config)->features;
        for(unsigned i=0;i<f->event_count && i<8;i++) {
            automation_task_t *t=&next->tasks[i];t->id=next->next_id++;t->days=f->events[i].days;t->minute=f->events[i].minute;
            t->action.kind=AUTO_FAN;t->action.value=f->events[i].on;t->action.speed=f->events[i].speed;
            t->enabled=1;t->state=AUTO_WAITING;
        }
        a1_storage_automation_put((app_storage_t *)a1_config, next);free(next);
        f->event_count=0;f->scheduled_off=f->scheduled_valid=0;
        if (mico_system_context_update(a1_context) != kNoErr) return kUnknownErr;
    }
    /* Normalize the retained ABI byte without changing credentials or layout. */
    if (a1_config->mqtt_topic_mode != MQTT_TOPIC_TMCP) {
        a1_config->mqtt_topic_mode = MQTT_TOPIC_TMCP;
        if (mico_system_context_update(a1_context) != kNoErr) return kUnknownErr;
    }
    a1_config_get(&c);
    if (c.magic != A1_CONFIG_MAGIC || c.version != A1_CONFIG_VERSION) {
        service_event("error","config","unsupported_layout");
        return kUnknownErr;
    }
    feature_config_storage_t *extension = &((app_storage_t *)a1_config)->features;
    if (extension->magic != FEATURE_MAGIC) {
        a1_storage_features_defaults((app_storage_t *)a1_config);
        if (mico_system_context_update(a1_context) != kNoErr) return startup_result("config persist", kUnknownErr);
    }
    err = startup_result("MiCO system", mico_system_init(a1_context));
    if (err != kNoErr) return err;
    uint8_t mac[6]; mico_wlan_get_mac_address(mac);
    snprintf(a1_id, sizeof(a1_id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    bemfa_config_t *b=&((app_storage_t *)a1_config)->bemfa;
    if(b->magic!=BEMFA_MAGIC || b->version!=1) {
        bemfa_defaults(b,"a1",a1_id);
        if(mico_system_context_update(a1_context)!=kNoErr)return startup_result("MQTT extension persist",kUnknownErr);
    }
    a1_config_get(&c);
    if (!c.name[0]) {
        snprintf(c.name, sizeof(c.name), "A1-%02X%02X", mac[4], mac[5]);
        if (!a1_config_save(&c)) return startup_result("config persist", kUnknownErr);
    }
    snprintf(a1_state.ap_name, sizeof(a1_state.ap_name), "A1-AP-%s", a1_id + 6);
    for (char *p = a1_state.ap_name; *p; ++p)
        if (*p >= 'a' && *p <= 'f') *p -= 'a' - 'A';
    strcpy(a1_state.ota_state, "idle");
    a1_state.led_until = mico_rtos_get_time() + 2000;
    err = startup_result("features", feature_start()); if (err != kNoErr) return err;
    err = startup_result("HTTP", a1_http_start()); if (err != kNoErr) return err;
    /* Keep provisioning available when peripheral initialization fails. */
    err = startup_result("hardware", a1_hardware_start());
    if (err != kNoErr) custom_log("A1", "Hardware unavailable; provisioning remains active");
    /* Start network last: its owner may launch MQTT after acquiring an IP.
     * Provisioning HTTP and local control must have their stacks first. */
    err = startup_result("network", a1_network_start()); if (err != kNoErr) return err;
    watchdog_startup_complete();
    custom_log("A1", "Started %s, id %s", A1_VERSION, a1_id);
    return kNoErr;
}

uint32_t feature_config_revision(void)
{
    mico_rtos_lock_mutex(&a1_config_lock);
    uint32_t revision = ((app_storage_t *)a1_config)->features.revision;
    mico_rtos_unlock_mutex(&a1_config_lock);
    return revision;
}

void feature_config_get(feature_config_t *out)
{
    mico_rtos_lock_mutex(&a1_config_lock);
    a1_storage_features_get((app_storage_t *)a1_config, out);
    feature_config_limit_peers(out);
    mico_rtos_unlock_mutex(&a1_config_lock);
}
bool feature_config_save(const feature_config_t *c)
{
    if (c->peer_count > FEATURE_MAX_PEERS) return false;
    feature_config_t *old = malloc(sizeof(*old));
    if (!old) return false;
    mico_rtos_lock_mutex(&a1_config_lock);
    app_storage_t *storage = (app_storage_t *)a1_config;
    feature_config_storage_t *live = &storage->features;
    mico_rtos_lock_mutex(&a1_lock);
    bool busy = a1_state.ota_active || a1_state.reboot_pending;
    mico_rtos_unlock_mutex(&a1_lock);
    bool ok = !busy && c->magic == FEATURE_MAGIC && live->revision == c->revision;
    if (ok) {
        a1_storage_features_get(storage, old); a1_storage_features_put(storage, c); live->revision++;
        ok = mico_system_context_update(a1_context) == kNoErr;
        if (!ok) a1_storage_features_put(storage, old);
    }
    mico_rtos_unlock_mutex(&a1_config_lock);
    free(old); return ok;
}

void automation_load(automation_config_t *out) {
    mico_rtos_lock_mutex(&a1_config_lock);a1_storage_automation_get((app_storage_t *)a1_config,out);out->timezone=((app_storage_t *)a1_config)->features.timezone_minutes;mico_rtos_unlock_mutex(&a1_config_lock);
}
bool automation_save(const automation_config_t *next) {
    automation_config_t *old=malloc(sizeof(*old));if(!old)return false;
    mico_rtos_lock_mutex(&a1_config_lock);
    app_storage_t *live=(app_storage_t *)a1_config;
    mico_rtos_lock_mutex(&a1_lock);bool busy=a1_state.ota_active || a1_state.reboot_pending || a1_state.wifi_trial_active;mico_rtos_unlock_mutex(&a1_lock);
    bool ok=!busy && next->magic==AUTOMATION_MAGIC && next->revision==live->automation.revision+1;
    if(ok){a1_storage_automation_get(live,old);a1_storage_automation_put(live,next);ok=mico_system_context_update(a1_context)==kNoErr;if(!ok)a1_storage_automation_put(live,old);}
    mico_rtos_unlock_mutex(&a1_config_lock);free(old);return ok;
}

#include "../common/device_status.h"
#include "../common/runtime_metrics.h"
void a1_status_capture(device_status_t *snapshot, device_status_full_t *full)
{
    memset(snapshot, 0, sizeof(*snapshot)); if(full)memset(full,0,sizeof(*full));
#ifdef PHICOMM_BEMFA
    bemfa_status_capture(&snapshot->network);
#endif
    snapshot->model=DEVICE_A1;
    snapshot->captured=STATUS_IDENTITY|STATUS_NETWORK|STATUS_OTA|STATUS_CONTROL;
    snapshot->identity.version=A1_VERSION; memcpy(snapshot->identity.id,a1_id,sizeof(a1_id));
    mico_rtos_lock_mutex(&a1_config_lock);
    mico_rtos_lock_mutex(&a1_lock);
    memcpy(snapshot->identity.name,a1_config->name,sizeof(snapshot->identity.name));
    if(full) {
        memcpy(full->network.ssid,a1_config->ssid,sizeof(full->network.ssid));
        memcpy(full->network.mqtt_host,a1_config->mqtt_host,sizeof(full->network.mqtt_host));
        full->network.mqtt_port = a1_config->mqtt_port;
        memcpy(full->network.mqtt_user,a1_config->mqtt_user,sizeof(full->network.mqtt_user));
    }
    snapshot->control.a1.fan_on = a1_config->fan_on;
    snapshot->control.a1.speed = a1_config->speed;
    snapshot->control.a1.led_enabled = a1_config->led_enabled;
    snapshot->control.a1.led_always_on = a1_config->led_always_on;
    snapshot->control.a1.brightness = a1_config->brightness;
    snapshot->control.a1.red = a1_config->red;
    snapshot->control.a1.green = a1_config->green;
    snapshot->control.a1.blue = a1_config->blue;
    snapshot->control.a1.child_lock = a1_config->child_lock;
    snapshot->network.wifi_trial_active = a1_state.wifi_trial_active;
    memcpy(snapshot->network.wifi_trial_status,a1_state.wifi_trial_status,sizeof(a1_state.wifi_trial_status));
    snapshot->network.mqtt_status = a1_state.mqtt_status;
    snapshot->network.mqtt_cleanup_warning = a1_state.mqtt_cleanup_warning;
    snapshot->network.wifi_connected = a1_state.wifi_connected;
    snapshot->network.mqtt_connected = a1_state.mqtt_connected;
    snapshot->network.ap_active = a1_state.ap_active;
    memcpy(snapshot->ota.state,a1_state.ota_state,sizeof(a1_state.ota_state));
    memcpy(snapshot->ota.error,a1_state.ota_error,sizeof(a1_state.ota_error));
    snapshot->ota.progress = a1_state.ota_progress;
    snapshot->ota.active = a1_state.ota_active;
    snapshot->ota.reboot_pending = a1_state.reboot_pending;
    snapshot->ota.scan_busy = a1_state.scan_busy;
    if(full) {
        memcpy(full->network.ip,a1_state.ip,sizeof(a1_state.ip));
        memcpy(full->network.ap_name,a1_state.ap_name,sizeof(a1_state.ap_name));
        full->network.mqtt_errors = a1_state.mqtt_errors;
        memcpy(full->network.mqtt_last_error,a1_state.mqtt_last_error,sizeof(a1_state.mqtt_last_error));
        full->ota.signature_verified = a1_state.ota_signature_verified;
        full->ota.image_verified = a1_state.ota_image_verified;
        full->ota.key_id = a1_state.ota_key_id;
        full->ota.image_length = a1_state.ota_image_length;
        memcpy(full->ota.release_counter,a1_state.ota_release_counter,sizeof(a1_state.ota_release_counter));
        memcpy(full->ota.version,a1_state.ota_version,sizeof(a1_state.ota_version));
    }
    snapshot->control.a1.applied_speed = a1_state.applied_speed;
    snapshot->control.a1.fan_ready = a1_state.fan_ready;
    snapshot->control.a1.touch_active = a1_state.touch_active;
    snapshot->control.a1.led_visible = a1_state.led_visible;
    snapshot->control.a1.panel_ready = a1_state.panel_ready;
    snapshot->control.a1.config_dirty = a1_state.config_dirty;
    memcpy(snapshot->control.a1.save_error,a1_state.save_error,sizeof(a1_state.save_error));
    if(full) {
        full->model.a1.panel_errors = a1_state.panel_errors;
        full->model.a1.hardware_errors = a1_state.hardware_errors;
        full->model.a1.panel_last_error = a1_state.panel_last_error;
        memcpy(full->model.a1.panel_raw,a1_state.panel_raw,sizeof(a1_state.panel_raw));
        full->network.web_password_set=a1_config->web_password[0]!=0;
        full->network.wifi_password_set=a1_config->wifi_password[0]!=0;
        full->network.mqtt_password_set=a1_config->mqtt_password[0]!=0;
    }
    mico_rtos_unlock_mutex(&a1_lock);
    mico_rtos_unlock_mutex(&a1_config_lock);
    feature_fan_snapshot(&snapshot->control.a1.effective_on,&snapshot->control.a1.effective_speed, &snapshot->control.a1.automatic,&snapshot->control.a1.auto_limit);
    snapshot->now_ms=mico_rtos_get_time(); /* after copying all timestamped state */
    if(full) {
        snapshot->captured|=STATUS_DIAGNOSTIC;
        runtime_metrics_capture(&full->runtime,snapshot->now_ms);
        watchdog_status_capture(&full->watchdog); full->http=ph_http_snapshot();
        a1_filter_get(&full->model.a1.filter);
    }
}

void bemfa_config_get(bemfa_config_t *out) {
    mico_rtos_lock_mutex(&a1_config_lock);*out=((app_storage_t *)a1_config)->bemfa;mico_rtos_unlock_mutex(&a1_config_lock);
}
bool bemfa_primary_enabled(void) {
    mico_rtos_lock_mutex(&a1_config_lock);bool enabled=((app_storage_t *)a1_config)->bemfa.mqtt_enabled;mico_rtos_unlock_mutex(&a1_config_lock);return enabled;
}
/* Commit base MQTT settings and switches together, with optimistic revisions. */
bool a1_mqtt_config_save(const a1_config_t *base,const bemfa_config_t *next) {
    mico_rtos_lock_mutex(&a1_config_lock);mico_rtos_lock_mutex(&a1_lock);
    bemfa_config_t *live=&((app_storage_t *)a1_config)->bemfa;
    bool valid=!a1_state.ota_active && !a1_state.reboot_pending && !a1_state.wifi_trial_active &&
        next->revision==live->revision+1 && (!base || base->revision==a1_config->revision);
    mico_rtos_unlock_mutex(&a1_lock);
    bool ok=false;
    if(valid) {
        bemfa_config_t old=*live;a1_config_t oldbase=*a1_config;
        *live=*next;if(base){*a1_config=*base;a1_config->revision++;}
        ok=flush_locked();if(!ok){*live=old;*a1_config=oldbase;}
        else {mico_rtos_lock_mutex(&a1_lock);a1_state.config_generation++;mico_rtos_unlock_mutex(&a1_lock);}
    }
    mico_rtos_unlock_mutex(&a1_config_lock);return ok;
}
bool bemfa_config_save(const bemfa_config_t *c) {return a1_mqtt_config_save(NULL,c);}
bool bemfa_network_ready(void) {
    mico_rtos_lock_mutex(&a1_lock);bool ready=a1_state.wifi_connected && !a1_state.ota_active && !a1_state.reboot_pending && !a1_state.wifi_trial_active;mico_rtos_unlock_mutex(&a1_lock);return ready;
}
const char *bemfa_device_id(void) {return a1_id;}

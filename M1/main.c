#include "../common/automation.h"

#include "../common/backup.h"
static bool config_recovery, config_commit_uncertain;
bool device_config_recovery(void) { return config_recovery || config_commit_uncertain; }
void device_config_recovery_set(bool recovery) { config_recovery=recovery || config_commit_uncertain; }
void app_config_commit_indeterminate(void) { config_commit_uncertain=config_recovery=true; }
bool app_config_persist_allowed(void) { return !config_recovery && !config_commit_uncertain; }
#include "../common/service.h"
#include "../common/watchdog.h"
#include "m1.h"
#include "config_legacy.h"
#include "storage.h"
#include <stddef.h>

mico_mutex_t m1_lock;
static mico_mutex_t config_lock;
static m1_config_t *app_config;

char m1_id[13];
m1_state_t m1_state;
mico_Context_t *m1_context;

#define AUTH_DEVICE_ID m1_id
#define AUTH_CONFIG_LOCK config_lock
#define AUTH_STORE (((app_storage_t *)app_config)->local_auth)
#define AUTH_PASSWORD (app_config->web_password)
#define AUTH_CONTEXT m1_context
#include "../common/local_auth_mico.inc"

void appRestoreDefault_callback(void *data, uint32_t size)
{
    if (size < sizeof(m1_config_t)) return;
    m1_config_t *c = data;
    memset(c, 0, sizeof(*c));
    if (size >= sizeof(app_storage_t)) { feature_config_t next; feature_defaults(&next); memcpy(&((app_storage_t *)data)->features, &next, sizeof(feature_config_storage_t)); }
    if (size >= sizeof(app_storage_t)) automation_defaults(&((app_storage_t *)data)->automation);
    if (size >= sizeof(app_storage_t)) memset(((app_storage_t *)data)->reserved_controller_config, 0, 20);
    if (size >= sizeof(app_storage_t)) ((app_storage_t *)data)->co2=(m1_co2_config_t){.magic=M1_CO2_MAGIC,.interval=5,.low=1000,.high=1500};
    if (size >= sizeof(app_storage_t)) ((app_storage_t *)data)->co2_source=(m1_co2_source_config_t){.magic=M1_CO2_SOURCE_MAGIC};
    if (size >= sizeof(app_storage_t)) m1_display_defaults(&((app_storage_t *)data)->display);
    if(size>=sizeof(app_storage_t))ph_auth_store_empty(&((app_storage_t *)data)->local_auth);
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

uint8_t device_language_get(void)
{
    mico_rtos_lock_mutex(&config_lock);
    uint8_t language = ((app_storage_t *)app_config)->features.language;
    mico_rtos_unlock_mutex(&config_lock);
    return language == PH_LANG_EN ? PH_LANG_EN : PH_LANG_ZH;
}

typedef struct {const m1_config_t *config;int language;} m1_config_commit_args;
static bool m1_config_commit(void *opaque,const ph_auth_store *auth_store)
{
    m1_config_commit_args *args=opaque;const m1_config_t *c=args->config;int language=args->language;
    mico_rtos_lock_mutex(&config_lock);
    if (device_config_recovery() || (c->revision != app_config->revision && c->magic == app_config->magic)) {
        mico_rtos_unlock_mutex(&config_lock); return false;
    }
    m1_config_t old = *app_config;
    const ph_auth_store *auth_before=device_local_auth()?&device_local_auth()->store:NULL;AUTH_STORE=*auth_store;
    memcpy(app_config, c, sizeof(*c));
    app_config->revision = old.revision + 1;
    feature_config_storage_t *features = &((app_storage_t *)app_config)->features;
    int16_t old_timezone = features->timezone_minutes;
    uint8_t old_language = features->language;
    if (language >= 0) features->language = language;
    features->timezone_minutes = c->timezone_minutes;
    OSStatus err = mico_system_context_update(m1_context);
    if (err != kNoErr) { if(auth_before)AUTH_STORE=*auth_before; memcpy(app_config, &old, sizeof(old)); features->timezone_minutes = old_timezone; features->language = old_language; }
    mico_rtos_unlock_mutex(&config_lock);
    if (err != kNoErr) return false;
    mico_rtos_lock_mutex(&m1_lock);
    m1_state.config_generation++;
    mico_rtos_unlock_mutex(&m1_lock);
    return true;
}

static bool m1_config_save_language(const m1_config_t *c,int language)
{
    m1_config_commit_args args={c,language};
    return device_local_auth()?ph_auth_credential_commit(device_local_auth(),c->web_password,false,m1_config_commit,&args):
        m1_config_commit(&args,&AUTH_STORE);
}

bool m1_config_save(const m1_config_t *c) { return m1_config_save_language(c, -1); }
bool m1_device_config_save(const m1_config_t *c, uint8_t language) {
    return language <= PH_LANG_EN && m1_config_save_language(c, language);
}

void m1_co2_config_get(m1_co2_config_t *out)
{
    mico_rtos_lock_mutex(&config_lock);
    *out=((app_storage_t *)app_config)->co2;
    mico_rtos_unlock_mutex(&config_lock);
}
void m1_co2_source_config_get(m1_co2_source_config_t *out)
{
    mico_rtos_lock_mutex(&config_lock);*out=((app_storage_t *)app_config)->co2_source;mico_rtos_unlock_mutex(&config_lock);
}
void m1_display_config_get(m1_display_config_t *out)
{
    mico_rtos_lock_mutex(&config_lock);*out=((app_storage_t *)app_config)->display;mico_rtos_unlock_mutex(&config_lock);
}
void m1_display_switch_get(m1_display_switch_config_t *out)
{
    mico_rtos_lock_mutex(&config_lock);
    const m1_display_config_t *c=&((app_storage_t *)app_config)->display;
    *out=(m1_display_switch_config_t){.revision=c->revision,.configured=c->configured,
        .selected=c->selected,.double_click=c->double_click,.formatted=c->reserved==M1_DISPLAY_FORMATTED};
    for(unsigned i=0;i<M1_DISPLAY_LANES;i++)if(c->lanes[i].source!=M1_DISPLAY_NONE)out->present=true;
    mico_rtos_unlock_mutex(&config_lock);
}
bool m1_display_lane_get(unsigned slot,m1_display_lane_config_t *out,m1_display_remote_t *remote,uint32_t *revision)
{
    if(slot>=M1_DISPLAY_LANES)return false;
    mico_rtos_lock_mutex(&config_lock);
    const m1_display_config_t *c=&((app_storage_t *)app_config)->display;
    *out=c->lanes[slot];*revision=c->revision;bool configured=c->configured;
    if(remote)*remote=c->remote[slot];
    mico_rtos_unlock_mutex(&config_lock);return configured;
}
const char *m1_display_config_save(const m1_display_config_t *next)
{
    if(!m1_display_config_valid(next))return "invalid_params";
    for(unsigned i=0;i<M1_DISPLAY_LANES;i++)
        if(next->lanes[i].source==M1_DISPLAY_MQTT && !strcmp(next->lanes[i].device_id,m1_id))return "invalid_params";
    mico_rtos_lock_mutex(&config_lock);
    m1_display_config_t *live=&((app_storage_t *)app_config)->display,old=*live;
    mico_rtos_lock_mutex(&m1_lock);
    bool busy=m1_state.ota_active||m1_state.mcu_updating||m1_state.reboot_pending||m1_state.wifi_trial_active;
    mico_rtos_unlock_mutex(&m1_lock);
    const char *code="busy";
    if(!busy && !config_recovery && next->revision==old.revision && old.revision<INT32_MAX){
        *live=*next;live->revision++;
        if(mico_system_context_update(m1_context)!=kNoErr){*live=old;code="storage_error";}else code="accepted";
    }
    if(!strcmp(code,"accepted"))m1_display_source_reset();
    mico_rtos_unlock_mutex(&config_lock);return code;
}
const char *m1_display_data_save(unsigned slot,uint32_t revision,float value,unsigned unit,unsigned ttl,int color)
{
    if(slot>=M1_DISPLAY_LANES || !isfinite(value) || value<0 || value>999999 || color < -1 || color>3)return "invalid_params";
    mico_rtos_lock_mutex(&config_lock);
    const m1_display_config_t *c=&((app_storage_t *)app_config)->display;
    const m1_display_lane_config_t *l=&c->lanes[slot];
    const char *code="invalid_params";
    mico_rtos_lock_mutex(&m1_lock);
    bool busy=m1_state.ota_active||m1_state.mcu_updating||m1_state.reboot_pending||m1_state.wifi_trial_active;
    mico_rtos_unlock_mutex(&m1_lock);
    if(busy || config_recovery || c->revision!=revision)code="busy";
    else if(c->configured && !c->reserved && l->source==M1_DISPLAY_API && l->unit==unit && ttl<=l->ttl_seconds &&
            (!ttl || m1_display_metric_value_valid(l->metric,value))){
        m1_display_value_t encoded;
        if(!ttl || m1_display_encode_value(l,slot,value,ttl,color<0?l->color:color,&encoded)){
            m1_display_data_store(slot,revision,value,ttl,color);code="accepted";
        }
    }
    mico_rtos_unlock_mutex(&config_lock);return code;
}
const char *m1_display_frame_save(unsigned slot,uint32_t revision,unsigned number,unsigned format,unsigned lamps,unsigned ttl)
{
    if(slot>=M1_DISPLAY_LANES || number>999 || format>7 || (slot==1 && (format&1u)) ||
       lamps>M1_DISPLAY_LAMPS_MAX || ttl>60 || (!ttl && (number || format || lamps)))return "invalid_params";
    mico_rtos_lock_mutex(&config_lock);
    const m1_display_config_t *c=&((app_storage_t *)app_config)->display;
    const m1_display_lane_config_t *l=&c->lanes[slot];
    const char *code="invalid_params";
    mico_rtos_lock_mutex(&m1_lock);
    bool busy=m1_state.ota_active||m1_state.mcu_updating||m1_state.reboot_pending||m1_state.wifi_trial_active;
    mico_rtos_unlock_mutex(&m1_lock);
    if(busy || config_recovery || c->revision!=revision)code="busy";
    else if(c->configured && c->reserved==M1_DISPLAY_FORMATTED && l->source==M1_DISPLAY_API && ttl<=l->ttl_seconds){
        m1_display_frame_store(slot,revision,number,format,lamps,ttl);code="accepted";
    }
    mico_rtos_unlock_mutex(&config_lock);return code;
}
const char *m1_co2_source_config_save(const m1_co2_source_config_t *next)
{
    if(!m1_co2_source_config_valid(next) || (next->external&&!strcmp(next->device_id,m1_id)))return "invalid_params";
    mico_rtos_lock_mutex(&config_lock);
    m1_co2_source_config_t *live=&((app_storage_t *)app_config)->co2_source,old=*live;
    m1_display_config_t *display=&((app_storage_t *)app_config)->display;
    uint32_t old_display_revision=display->revision;bool old_display_configured=display->configured;
    mico_rtos_lock_mutex(&m1_lock);
    bool busy=m1_state.ota_active||m1_state.mcu_updating||m1_state.reboot_pending||m1_state.wifi_trial_active;
    mico_rtos_unlock_mutex(&m1_lock);
    const char *code="busy";
    if(!busy&&!config_recovery&&next->revision==old.revision&&old.revision<INT32_MAX&&
       (!display->configured||display->revision<INT32_MAX)){
        *live=*next;live->revision++;
        if(display->configured){display->configured=0;display->revision++;}
        if(mico_system_context_update(m1_context)!=kNoErr){*live=old;display->revision=old_display_revision;display->configured=old_display_configured;code="storage_error";}
        else {code="accepted";m1_display_source_reset();}
    }
    mico_rtos_unlock_mutex(&config_lock);
    if(!strcmp(code,"accepted"))m1_co2_source_reset();return code;
}
const char *m1_co2_config_save(const m1_co2_config_t *next)
{
    if(next->magic!=M1_CO2_MAGIC || next->mode>2 || next->slot>1 || !next->interval || next->interval>3600 || next->low>next->high)return "invalid_params";
    mico_rtos_lock_mutex(&config_lock);
    m1_co2_config_t *live=&((app_storage_t *)app_config)->co2,old=*live;
    m1_display_config_t *display=&((app_storage_t *)app_config)->display;
    uint32_t old_display_revision=display->revision;bool old_display_configured=display->configured;
    mico_rtos_lock_mutex(&m1_lock);
    bool busy=m1_state.ota_active || m1_state.mcu_updating || m1_state.reboot_pending || m1_state.wifi_trial_active;
    mico_rtos_unlock_mutex(&m1_lock);
    const char *code="busy";
    if(!busy && !config_recovery && old.revision==next->revision && old.revision<INT32_MAX &&
       (!display->configured||display->revision<INT32_MAX)) {
        *live=*next;live->revision++;
        if(display->configured){display->configured=0;display->revision++;}
        if(mico_system_context_update(m1_context)!=kNoErr){*live=old;display->revision=old_display_revision;display->configured=old_display_configured;code="storage_error";}
        else {code="accepted";m1_display_source_reset();}
    }
    mico_rtos_unlock_mutex(&config_lock);return code;
}

static void alarm_load_locked(m1_alarm_config_t *out)
{
    memcpy(out, ((app_storage_t *)app_config)->reserved_controller_config, sizeof(*out));
    if (out->magic != M1_ALARM_MAGIC || (out->configured & ~3u) ||
        ((out->configured & 1) && (!out->hcho[1] || out->hcho[0] > out->hcho[1])) ||
        ((out->configured & 2) && (!out->pm25[1] || out->pm25[0] > out->pm25[1])))
        memset(out, 0, sizeof(*out));
    /* Older threshold-only records retain their limits and revision. A new
     * or unrecognised alarm extension always starts with both buzzers off. */
    if (out->reserved[0] != M1_ALARM_SCHEMA || (out->reserved[1] & ~3u) || out->reserved[2]) {
        out->reserved[0] = M1_ALARM_SCHEMA;
        out->reserved[1] = out->reserved[2] = 0;
    }
}

void m1_alarm_get(m1_alarm_config_t *out)
{
    mico_rtos_lock_mutex(&config_lock);
    alarm_load_locked(out);
    mico_rtos_unlock_mutex(&config_lock);
}

const char *m1_alarm_set(bool pm25, uint16_t low, uint16_t high, uint32_t revision, int buzzer)
{
    if (!high || low > high || buzzer < -1 || buzzer > 1) return "invalid_params";
    mico_rtos_lock_mutex(&config_lock);
    m1_alarm_config_t next;
    alarm_load_locked(&next);
    mico_rtos_lock_mutex(&m1_lock);
    bool busy = m1_state.ota_active || m1_state.mcu_updating || m1_state.reboot_pending || m1_state.wifi_trial_active;
    mico_rtos_unlock_mutex(&m1_lock);
    const char *result = "busy";
    /* Keep the revision representable by the SDK's signed JSON integer. */
    if (!busy && next.revision == revision && revision < INT32_MAX) {
        uint8_t old[20];
        uint8_t *live = ((app_storage_t *)app_config)->reserved_controller_config;
        memcpy(old, live, sizeof(old));
        uint16_t *pair = pm25 ? next.pm25 : next.hcho;
        pair[0] = low; pair[1] = high;
        next.magic = M1_ALARM_MAGIC; next.revision++;
        next.configured |= pm25 ? 2 : 1;
        if (buzzer >= 0) {
            unsigned mask = pm25 ? 2u : 1u;
            next.reserved[1] = (next.reserved[1] & ~mask) | (buzzer ? mask : 0);
        }
        memcpy(live, &next, sizeof(next));
        if (mico_system_context_update(m1_context) != kNoErr) {
            memcpy(live, old, sizeof(old)); result = "storage_error";
        } else {
            mico_rtos_lock_mutex(&m1_lock);
            m1_state.alarm_uart_sent = false;
            mico_rtos_unlock_mutex(&m1_lock);
            result = "accepted";
        }
    }
    mico_rtos_unlock_mutex(&config_lock);
    return result;
}

static bool m1_reset_commit(void *opaque,const ph_auth_store *auth_store)
{
    bool full=*(bool *)opaque;
    app_storage_t *old = malloc(sizeof(*old));
    if (!old) return false;
    mico_rtos_lock_mutex(&config_lock);
    /* A guarded SDK update can skip persistence with kNoErr. Never ACK a
     * network-only reset in recovery; clearing needs explicit full consent. */
    if (!full && device_config_recovery()) {
        mico_rtos_unlock_mutex(&config_lock);
        free(old);
        return false;
    }
    mico_rtos_lock_mutex(&m1_lock);
    bool ok = !m1_state.ota_active && !m1_state.mcu_updating && !m1_state.reboot_pending && !m1_state.wifi_trial_active;
    if (ok) { m1_state.reboot_pending = true; m1_state.reboot_at = mico_rtos_get_time() + 0x7fffffffu; }
    mico_rtos_unlock_mutex(&m1_lock);
    if (ok) {
        memcpy(old, app_config, sizeof(*old));
        bool was_recovery=config_recovery;
        if(full)config_recovery=false;
        if (full) appRestoreDefault_callback(app_config, sizeof(*old));
        else { app_config->ssid[0] = 0; app_config->wifi_password[0] = 0; app_config->web_password[0] = 0; }
        AUTH_STORE=*auth_store;
        ok = mico_system_context_update(m1_context) == kNoErr;
        if (!ok) { memcpy(app_config, old, sizeof(*old)); config_recovery=was_recovery || config_commit_uncertain; }
        mico_rtos_lock_mutex(&m1_lock);
        m1_state.reboot_pending = ok;
        if (ok) m1_state.reboot_at = mico_rtos_get_time() + 1000u;
        mico_rtos_unlock_mutex(&m1_lock);
    }
    mico_rtos_unlock_mutex(&config_lock);
    free(old);
    return ok;
}

bool m1_reset(bool full)
{
    const ph_auth_store *empty=ph_auth_store_empty_value();
    return device_local_auth()?ph_auth_credential_commit(device_local_auth(),"",true,m1_reset_commit,&full):
        m1_reset_commit(&full,empty);
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
    bool ok = !m1_state.ota_active && !m1_state.mcu_updating && !m1_state.reboot_pending && !m1_state.wifi_trial_active;
    if (ok) { m1_state.reboot_pending = true; m1_state.reboot_at = mico_rtos_get_time() + ms; }
    mico_rtos_unlock_mutex(&m1_lock);
    return ok;
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
    /* MiCO validates its partition, not the previous firmware's app layout.
     * Reset only foreign application data; preserve the SDK network settings. */
    if (app_config->magic != M1_CONFIG_MAGIC || app_config->version < 1 || app_config->version > M1_CONFIG_VERSION ||
        (app_config->version >= 8 && !m1_co2_source_config_valid(&((app_storage_t *)app_config)->co2_source)) ||
        (app_config->version == 9 && !m1_display_v9_valid(&((app_storage_t *)app_config)->display)) ||
        (app_config->version == 10 && !m1_display_v10_valid(&((app_storage_t *)app_config)->display)) ||
        (app_config->version == M1_CONFIG_VERSION && !m1_display_config_valid(&((app_storage_t *)app_config)->display))) {
        config_recovery=true;
        memset(app_config,0,sizeof(app_storage_t));
        appRestoreDefault_callback(app_config,sizeof(app_storage_t));
        service_event("error","config","unsupported_layout");
    }
    bool migration_changed=false;
    bool controller_migration = app_config->version < 6;
    bool co2_migration = app_config->version < 7;
    bool source_migration = app_config->version < 8;
    bool display_migration = app_config->version < 9;
    bool presentation_migration = app_config->version == 9;
    bool field_migration = app_config->version == 9 || app_config->version == 10;
    if (app_config->magic == M1_CONFIG_MAGIC &&
        (app_config->version == 1 || app_config->version == 2)) {
        m1_storage_v2_t *old = malloc(sizeof(*old));
        if (!old) return kNoMemoryErr;
        memcpy(old, app_config, sizeof(*old));
        memset(app_config, 0, sizeof(app_storage_t));
        feature_config_t next; m1_migrate_v2(app_config, &next, old);
        memcpy(&((app_storage_t *)app_config)->features, &next, sizeof(feature_config_storage_t));
        free(old);
        { feature_config_t next; memcpy(&next, &((app_storage_t *)app_config)->features, sizeof(feature_config_storage_t));
        m1_migrate_response_permissions(app_config, &next); memcpy(&((app_storage_t *)app_config)->features, &next, sizeof(feature_config_storage_t)); }
        migration_changed=true;
    }
    if (app_config->magic == M1_CONFIG_MAGIC && app_config->version == 3) {
        { feature_config_t next; memcpy(&next, &((app_storage_t *)app_config)->features, sizeof(feature_config_storage_t));
        m1_migrate_response_permissions(app_config, &next); memcpy(&((app_storage_t *)app_config)->features, &next, sizeof(feature_config_storage_t)); }
        migration_changed=true;
    }
    if (app_config->magic == M1_CONFIG_MAGIC && app_config->version == 4) {
        ((app_storage_t *)app_config)->automation.magic=0;
        app_config->version = M1_CONFIG_VERSION;
        migration_changed=true;
    }
    if (((app_storage_t *)app_config)->automation.magic != AUTOMATION_MAGIC) {
        automation_config_t *next=&((app_storage_t *)app_config)->automation;
        automation_defaults(next);
        feature_config_storage_t *f=&((app_storage_t *)app_config)->features;
        for(unsigned i=0;i<f->event_count && i<8;i++) {
            automation_task_t *t=&next->tasks[i];t->id=next->next_id++;t->days=f->events[i].days;t->minute=f->events[i].minute;
            t->action.kind=AUTO_FAN;t->action.value=f->events[i].on;t->action.speed=f->events[i].speed;
            t->enabled=0;t->state=AUTO_MIGRATED_DISABLED;
        }
        f->event_count=0;f->scheduled_off=f->scheduled_valid=0;
        migration_changed=true;
    }
    if (controller_migration) {
        memset(((app_storage_t *)app_config)->reserved_controller_config, 0, 20);
        app_config->version = M1_CONFIG_VERSION;
        migration_changed=true;
    }
    if (co2_migration) {
        ((app_storage_t *)app_config)->co2=(m1_co2_config_t){.magic=M1_CO2_MAGIC,.interval=5,.low=1000,.high=1500};
        app_config->version=M1_CONFIG_VERSION;
        migration_changed=true;
    }
    if(source_migration){
        ((app_storage_t *)app_config)->co2_source=(m1_co2_source_config_t){.magic=M1_CO2_SOURCE_MAGIC};
        ((app_storage_t *)app_config)->features.language=0;
        app_config->version=M1_CONFIG_VERSION;migration_changed=true;
    }
    if(display_migration){
        /* A v8 installation keeps its CO2 profile and behavior. The new dual
         * profile becomes active only after an explicit full configuration. */
        m1_display_defaults(&((app_storage_t *)app_config)->display);
        app_config->version=M1_CONFIG_VERSION;migration_changed=true;
    }
    if(presentation_migration){
        /* A v9 dual/legacy profile retains its exact source lanes and active
         * behavior. New presentation options require an explicit profile save. */
        m1_display_upgrade_v9(&((app_storage_t *)app_config)->display);
        app_config->version=M1_CONFIG_VERSION;migration_changed=true;
    }
    if(field_migration){
        /* Preserve the complete old profile; append its stable field binding
         * without reading arbitrary bytes beyond the former layout. */
        m1_display_upgrade_remote(&((app_storage_t *)app_config)->display);
        app_config->version=M1_CONFIG_VERSION;migration_changed=true;
    }
    /* Normalize the retained ABI byte without changing credentials or layout. */
    if (app_config->mqtt_topic_mode != MQTT_TOPIC_TMCP) {
        app_config->mqtt_topic_mode = MQTT_TOPIC_TMCP;
        migration_changed=true;
    }
    m1_config_get(&c);
    if (c.magic != M1_CONFIG_MAGIC || c.version != M1_CONFIG_VERSION) {
        service_event("error","config","unsupported_layout");
        return kUnknownErr;
    }
    feature_config_storage_t *extension = &((app_storage_t *)app_config)->features;
    if (extension->magic != FEATURE_MAGIC) {
        feature_config_t next; feature_defaults(&next); memcpy(extension, &next, sizeof(*extension));
        migration_changed=true;
    }
    if(migration_changed && !config_recovery && mico_system_context_update(m1_context)!=kNoErr){
        /* Boot remains available for explicit recovery. No later SDK/app write
         * may overwrite the original layout after a failed migration. */
        config_recovery=true;
        memset(app_config,0,sizeof(app_storage_t));
        appRestoreDefault_callback(app_config,sizeof(app_storage_t));
        m1_config_get(&c);
        service_event("error","config","migration_failed");
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
    if(!device_local_auth_init())return kNoMemoryErr;
    err = feature_start(); if (err != kNoErr) return err;
    err = m1_uart_start(); if (err != kNoErr) return err;
    err = m1_http_start(); if (err != kNoErr) return err;
    err = m1_network_start(); if (err != kNoErr) return err;
    watchdog_startup_complete();
    custom_log("M1", "Started %s, id %s", M1_VERSION, m1_id);
    return kNoErr;
}

uint32_t feature_config_revision(void)
{
    mico_rtos_lock_mutex(&config_lock);
    uint32_t revision = ((app_storage_t *)app_config)->features.revision;
    mico_rtos_unlock_mutex(&config_lock);
    return revision;
}

void feature_config_get(feature_config_t *out)
{
    mico_rtos_lock_mutex(&config_lock);
    memset(out, 0, sizeof(*out));
    memcpy(out, &((app_storage_t *)app_config)->features, sizeof(feature_config_storage_t));
    feature_config_limit_peers(out);
    mico_rtos_unlock_mutex(&config_lock);
}
bool feature_config_save(const feature_config_t *c)
{
    if (c->peer_count > FEATURE_MAX_PEERS) return false;
    feature_config_storage_t *old = malloc(sizeof(*old));
    if (!old) return false;
    mico_rtos_lock_mutex(&config_lock);
    feature_config_storage_t *live = &((app_storage_t *)app_config)->features;
    mico_rtos_lock_mutex(&m1_lock);
    bool busy = m1_state.ota_active || m1_state.mcu_updating || m1_state.reboot_pending;
    mico_rtos_unlock_mutex(&m1_lock);
    bool ok = !busy && c->magic == FEATURE_MAGIC && live->revision == c->revision;
    if (ok) {
        int16_t previous_timezone = app_config->timezone_minutes;
        uint32_t previous_revision = app_config->revision;
        *old = *live; memcpy(live, c, sizeof(*live)); live->revision++;
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
    mico_rtos_lock_mutex(&m1_lock);bool busy=m1_state.ota_active || m1_state.mcu_updating || m1_state.reboot_pending || m1_state.wifi_trial_active;mico_rtos_unlock_mutex(&m1_lock);
    bool ok=!busy && next->magic==AUTOMATION_MAGIC && next->revision==live->revision+1;
    if(ok){*old=*live;*live=*next;ok=mico_system_context_update(m1_context)==kNoErr;if(!ok)*live=*old;}
    mico_rtos_unlock_mutex(&config_lock);free(old);return ok;
}

#include "../common/device_status.h"
#include "../common/runtime_metrics.h"
void m1_status_capture(device_status_t *snapshot, device_status_full_t *full)
{
    memset(snapshot, 0, sizeof(*snapshot)); if(full)memset(full,0,sizeof(*full));
    snapshot->model=DEVICE_M1;
    snapshot->captured=STATUS_IDENTITY|STATUS_NETWORK|STATUS_OTA|STATUS_CONTROL|STATUS_SENSOR;
    snapshot->identity.version=M1_VERSION; memcpy(snapshot->identity.id,m1_id,sizeof(m1_id));
    mico_rtos_lock_mutex(&config_lock);
    mico_rtos_lock_mutex(&m1_lock);
    memcpy(snapshot->identity.name,app_config->name,sizeof(snapshot->identity.name));
    if(full) {
        memcpy(full->network.ssid,app_config->ssid,sizeof(full->network.ssid));
        memcpy(full->network.mqtt_host,app_config->mqtt_host,sizeof(full->network.mqtt_host));
        full->network.mqtt_port = app_config->mqtt_port;
        memcpy(full->network.mqtt_user,app_config->mqtt_user,sizeof(full->network.mqtt_user));
        full->model.m1.timezone_minutes = app_config->timezone_minutes;
    }
    snapshot->network.wifi_trial_active = m1_state.wifi_trial_active;
    memcpy(snapshot->network.wifi_trial_status,m1_state.wifi_trial_status,sizeof(m1_state.wifi_trial_status));
    snapshot->network.mqtt_status = m1_state.mqtt_status;
    snapshot->network.mqtt_cleanup_warning = m1_state.mqtt_cleanup_warning;
    snapshot->network.wifi_connected = m1_state.wifi_connected;
    snapshot->network.mqtt_connected = m1_state.mqtt_connected;
    snapshot->network.ap_active = m1_state.ap_active;
    memcpy(snapshot->ota.state,m1_state.ota_state,sizeof(m1_state.ota_state));
    memcpy(snapshot->ota.error,m1_state.ota_error,sizeof(m1_state.ota_error));
    snapshot->ota.progress = m1_state.ota_progress;
    snapshot->ota.active = m1_state.ota_active;
    snapshot->ota.reboot_pending = m1_state.reboot_pending;
    snapshot->ota.scan_busy = m1_state.scan_busy;
    if(full) {
        memcpy(full->network.ip,m1_state.ip,sizeof(m1_state.ip));
        memcpy(full->network.ap_name,m1_state.ap_name,sizeof(m1_state.ap_name));
        full->network.mqtt_errors = m1_state.mqtt_errors;
        memcpy(full->network.mqtt_last_error,m1_state.mqtt_last_error,sizeof(m1_state.mqtt_last_error));
        full->ota.signature_verified = m1_state.ota_signature_verified;
        full->ota.image_verified = m1_state.ota_image_verified;
        full->ota.key_id = m1_state.ota_key_id;
        full->ota.image_length = m1_state.ota_image_length;
        memcpy(full->ota.release_counter,m1_state.ota_release_counter,sizeof(m1_state.ota_release_counter));
        memcpy(full->ota.version,m1_state.ota_version,sizeof(m1_state.ota_version));
    }
    snapshot->sensor.m1.uart_ready = m1_state.uart_ready;
    snapshot->control.m1.brightness_requested = m1_state.brightness_requested;
    snapshot->sensor.m1.sample = m1_state.sample;
    snapshot->sensor.m1.barometer = m1_state.barometer;
    if(full) {
        memcpy(full->model.m1.ntp_status,m1_state.ntp_status,sizeof(m1_state.ntp_status));
        memcpy(full->model.m1.ntp_server,m1_state.ntp_server,sizeof(m1_state.ntp_server));
        full->model.m1.ntp_synced = m1_state.ntp_synced;
        full->model.m1.ntp_screen_queued = m1_state.ntp_screen_queued;
        full->model.m1.ntp_last_success = m1_state.ntp_last_success;
        full->model.m1.ntp_next_at = m1_state.ntp_next_at;
        full->model.m1.rx_frames = m1_state.rx_frames;
        full->model.m1.unknown_frames = m1_state.unknown_frames;
        full->model.m1.uart_errors = m1_state.uart_errors;
        full->model.m1.discarded_bytes = m1_state.discarded_bytes;
        memcpy(full->model.m1.last_frame,m1_state.last_frame,sizeof(m1_state.last_frame));
        full->model.m1.co2_samples = m1_state.scd40.samples;
        full->model.m1.co2_crc_errors = m1_state.scd40.crc_errors;
        full->model.m1.co2_bus_initialized = m1_state.scd40.bus_initialized;
        full->model.m1.co2_last_command = m1_state.scd40.last_command;
        full->model.m1.co2_error_phase = m1_state.scd40.error_phase;
    }
    snapshot->sensor.m1.co2.ready = m1_state.scd40.ready;
    snapshot->sensor.m1.co2.received = m1_state.scd40.received;
    snapshot->sensor.m1.co2.co2_ppm = m1_state.scd40.co2_ppm;
    snapshot->sensor.m1.co2.updated_ms = m1_state.scd40.updated_ms;
    snapshot->sensor.m1.co2.errors = m1_state.scd40.errors;
    snapshot->sensor.m1.co2.last_error = m1_state.scd40.last_error;
    memcpy(snapshot->sensor.m1.co2.serial,m1_state.scd40.serial,sizeof(m1_state.scd40.serial));
    if(full) {
        full->network.web_password_set=app_config->web_password[0]!=0;
        full->network.wifi_password_set=app_config->wifi_password[0]!=0;
        full->network.mqtt_password_set=app_config->mqtt_password[0]!=0;
        memcpy(full->model.m1.ntp_host,((app_storage_t *)app_config)->features.ntp_host,64); full->model.m1.ntp_host[63]=0;
    }
    mico_rtos_unlock_mutex(&m1_lock);
    mico_rtos_unlock_mutex(&config_lock);
    m1_co2_capture(&snapshot->sensor.m1.co2_display);
    snapshot->now_ms=mico_rtos_get_time(); /* after copying all timestamped state */
    if(full) {
        snapshot->captured|=STATUS_DIAGNOSTIC;
    }
}

void m1_diagnostics_capture(device_status_t *snapshot,device_status_full_t *full)
{
    m1_status_capture(snapshot,full);
    runtime_metrics_capture(&full->runtime,snapshot->now_ms);
    watchdog_status_capture(&full->watchdog);full->http=ph_http_snapshot();
}

#define BACKUP_MODEL "m1"
#define BACKUP_BASE m1_config_t
#define BACKUP_MAGIC M1_CONFIG_MAGIC
#define BACKUP_VERSION M1_CONFIG_VERSION
#define BACKUP_LIVE app_config
#define BACKUP_LOCK config_lock
#define BACKUP_CONTEXT m1_context
static bool backup_reserve(void) {
    mico_rtos_lock_mutex(&m1_lock);
    bool ok=!m1_state.ota_active && !m1_state.reboot_pending && !m1_state.wifi_trial_active && !m1_state.mcu_updating;
    if(ok){m1_state.reboot_pending=true;m1_state.reboot_at=mico_rtos_get_time()+0x7fffffffu;}
    mico_rtos_unlock_mutex(&m1_lock);return ok;
}
static void backup_finish(bool ok) {
    mico_rtos_lock_mutex(&m1_lock);m1_state.reboot_pending=ok;
    if(ok)m1_state.reboot_at=mico_rtos_get_time()+1500;
    mico_rtos_unlock_mutex(&m1_lock);
}
#include "../common/backup_mico.inc"

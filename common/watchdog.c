#include "watchdog.h"
#include "mico.h"
#include "service.h"
#include <string.h>

#if defined(MICO_SYSTEM_MONITOR_ENABLE) || defined(MICO_DISABLE_WATCHDOG)
#error "Application watchdog requires exclusive ownership of the hardware WDG"
#endif

/* MX1290 MOC reset bits, from the pinned SDK moc_api.h. Read once per boot. */
#define RESET_POWER 0x07
#define RESET_SOFTWARE 0x08
#define RESET_LOCKUP 0x10
#define RESET_WATCHDOG 0x20
extern int get_last_reset_reason(void);

typedef struct {
    uint32_t updated, delay;
    bool active, seen, deferred;
} heartbeat_t;
static heartbeat_t points[WATCHDOG_SLOT_COUNT];
static const char *const names[] = {"hardware", "network", "binding", "ota"};
static bool started, enabled, startup_complete, failed;
static uint32_t started_at;
static int reset_raw;
static const char *fault = "";

static uint32_t normal_delay(watchdog_slot_t slot)
{
    return slot == WATCHDOG_NETWORK ? WATCHDOG_NETWORK_MS :
        slot == WATCHDOG_OTA ? WATCHDOG_OTA_MS : WATCHDOG_LOCAL_MS;
}

/* The mocOS port declares but does not implement MiCO critical-section APIs.
 * Preserve PRIMASK so callers never accidentally enable masked interrupts. */
static uint32_t watchdog_lock(void)
{
#ifdef WATCHDOG_HOST
    mico_rtos_enter_critical();
    return 0;
#else
    uint32_t mask;
    __asm volatile ("mrs %0, primask\n\tcpsid i" : "=r" (mask) :: "memory");
    return mask;
#endif
}

static void watchdog_unlock(uint32_t mask)
{
#ifdef WATCHDOG_HOST
    (void)mask;
    mico_rtos_exit_critical();
#else
    __asm volatile ("msr primask, %0" :: "r" (mask) : "memory");
#endif
}

/* All state access is in short thread-context critical sections. No business
 * mutex, allocation, logging or Flash access may enter the feeding path. */
static bool healthy(uint32_t now)
{
    if (failed) return false;
    bool seen = true;
    for (unsigned i = 0; i < WATCHDOG_SLOT_COUNT; i++) {
        heartbeat_t *p = &points[i];
        if (!p->active) continue;
        seen &= p->seen;
        if ((uint32_t)(now - p->updated) >= p->delay) {
            failed = true; fault = names[i]; return false;
        }
    }
    if ((!startup_complete || !seen) &&
        (uint32_t)(now - started_at) >= WATCHDOG_STARTUP_MS) {
        failed = true; fault = "startup"; return false;
    }
    return true;
}

static void watchdog_poll(void)
{
    uint32_t mask = watchdog_lock();
    uint32_t now = mico_rtos_get_time();
    if (enabled && healthy(now)) MicoWdgReload();
    watchdog_unlock(mask);
}

static void monitor(mico_thread_arg_t arg)
{
    UNUSED_PARAMETER(arg);
    for (;;) {
        watchdog_poll();
        mico_thread_msleep(WATCHDOG_POLL_MS);
    }
}

int watchdog_start(void)
{
    reset_raw = get_last_reset_reason();
    started_at = mico_rtos_get_time();
    memset(points, 0, sizeof(points));
    for (unsigned i = 0; i < WATCHDOG_OTA; i++) {
        points[i].active = true;
        points[i].updated = started_at;
        points[i].delay = WATCHDOG_STARTUP_MS;
    }
    started = true;
    OSStatus err = MicoWdgInitialize(WATCHDOG_HARDWARE_MS);
    if (err != kNoErr) { failed = true; fault = "initialize"; return err; }
    enabled = true;
    err = mico_rtos_create_thread(NULL, 0, "watchdog", monitor, 1024, 0);
    if (err != kNoErr) { failed = true; fault = "monitor"; }
    return err;
}

void watchdog_startup_complete(void)
{
    uint32_t mask = watchdog_lock();
    uint32_t now = mico_rtos_get_time();
    if (started && healthy(now)) startup_complete = true;
    watchdog_unlock(mask);
}

void watchdog_beat(watchdog_slot_t slot)
{
    uint32_t mask = watchdog_lock();
    uint32_t now = mico_rtos_get_time();
    if (started && (unsigned)slot < WATCHDOG_OTA && healthy(now)) {
        heartbeat_t *p = &points[slot];
        /* A normal heartbeat must not renew an outstanding operation lease. */
        if (!p->deferred) {
            p->updated = now; p->delay = normal_delay(slot); p->seen = true;
        }
    }
    watchdog_unlock(mask);
}

bool watchdog_network_begin(void)
{
    bool ok = true;
    uint32_t mask = watchdog_lock();
    uint32_t now = mico_rtos_get_time();
    if (started) {
        heartbeat_t *p = &points[WATCHDOG_NETWORK];
        ok = healthy(now) && p->seen && !p->deferred;
        if (ok) { p->updated = now; p->delay = WATCHDOG_NTP_MS; p->deferred = true; }
    }
    watchdog_unlock(mask);
    return ok;
}

void watchdog_network_end(void)
{
    uint32_t mask = watchdog_lock();
    uint32_t now = mico_rtos_get_time();
    if (started && healthy(now)) {
        heartbeat_t *p = &points[WATCHDOG_NETWORK];
        if (p->deferred) {
            p->deferred = false; p->updated = now; p->delay = WATCHDOG_NETWORK_MS;
        }
    }
    watchdog_unlock(mask);
}

bool watchdog_ota_begin(void)
{
    bool ok = true;
    uint32_t mask = watchdog_lock();
    uint32_t now = mico_rtos_get_time();
    if (started) {
        heartbeat_t *p = &points[WATCHDOG_OTA];
        ok = healthy(now) && !p->active;
        if (ok) { p->active = p->seen = true; p->updated = now; p->delay = WATCHDOG_OTA_MS; }
    }
    watchdog_unlock(mask);
    return ok;
}

bool watchdog_ota_progress(void)
{
    bool ok = true;
    uint32_t mask = watchdog_lock();
    uint32_t now = mico_rtos_get_time();
    if (started) {
        ok = healthy(now) && points[WATCHDOG_OTA].active;
        if (ok) points[WATCHDOG_OTA].updated = now;
    }
    watchdog_unlock(mask);
    /* Also allow the lower priority business threads to run between sectors
     * and readback blocks. This never reloads the hardware watchdog. */
    mico_thread_msleep(1);
    return ok;
}

void watchdog_ota_end(void)
{
    uint32_t mask = watchdog_lock();
    uint32_t now = mico_rtos_get_time();
    if (started && healthy(now)) points[WATCHDOG_OTA].active = false;
    watchdog_unlock(mask);
}

static const char *reset_name(void)
{
    if (reset_raw < 0) return "unknown";
    if (reset_raw & RESET_WATCHDOG) return "watchdog";
    if (reset_raw & RESET_LOCKUP) return "lockup";
    if (reset_raw & RESET_SOFTWARE) return "software";
    if (reset_raw & RESET_POWER) return "power_on";
    return "unknown";
}

void watchdog_boot_event(void)
{
    custom_log("Watchdog", "Reset reason %s (0x%lx)", reset_name(), (unsigned long)reset_raw);
    service_event((reset_raw >= 0 && (reset_raw & RESET_WATCHDOG)) ? "warn" : "info",
                  "system", reset_name());
}

void watchdog_status_capture(watchdog_status_t *out)
{
    uint32_t mask = watchdog_lock();
    out->enabled = enabled; out->failed = failed; out->ready = startup_complete;
    for (unsigned i = 0; i < WATCHDOG_OTA; i++) out->ready &= points[i].seen;
    out->fault = fault; out->reset_raw = reset_raw; out->reset_name = reset_name();
    watchdog_unlock(mask);
}
void watchdog_status_encode(struct json_object *out, const watchdog_status_t *s)
{
    json_object_object_add(out, "reset_reason", json_object_new_string(s->reset_name));
    json_object_object_add(out, "reset_reason_raw", json_object_new_int(s->reset_raw));
    json_object_object_add(out, "watchdog_enabled", json_object_new_boolean(s->enabled));
    json_object_object_add(out, "watchdog_state", json_object_new_string(s->failed ? "fault" : s->ready ? "healthy" : "starting"));
    json_object_object_add(out, "watchdog_fault", json_object_new_string(s->fault));
}
void watchdog_status_json(struct json_object *out)
{
    watchdog_status_t snapshot; watchdog_status_capture(&snapshot);
    watchdog_status_encode(out, &snapshot);
}

void watchdog_binding_enable(bool on){
    uint32_t mask=watchdog_lock();heartbeat_t *p=&points[WATCHDOG_BINDING];
    p->active=on;p->updated=mico_rtos_get_time();p->deferred=false;p->delay=WATCHDOG_LOCAL_MS;
    watchdog_unlock(mask);
}

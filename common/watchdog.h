#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WATCHDOG_HARDWARE, WATCHDOG_NETWORK, WATCHDOG_BINDING, WATCHDOG_OTA,
    WATCHDOG_SLOT_COUNT
} watchdog_slot_t;

#define WATCHDOG_HARDWARE_MS 3000u
#define WATCHDOG_POLL_MS 500u
#define WATCHDOG_STARTUP_MS 60000u
#define WATCHDOG_LOCAL_MS 10000u
#define WATCHDOG_NETWORK_MS 30000u
#define WATCHDOG_NTP_MS 180000u
#define WATCHDOG_OTA_MS 30000u

/* Called once, before application initialization. Only the daemon reloads WDG. */
int watchdog_start(void);
void watchdog_boot_event(void);
void watchdog_startup_complete(void);
void watchdog_beat(watchdog_slot_t slot);
/* A bounded, non-renewable NTP allowance; end means the operation returned. */
bool watchdog_network_begin(void);
void watchdog_network_end(void);
/* OTA watches synchronous work only, never time spent awaiting network input. */
bool watchdog_ota_begin(void);
bool watchdog_ota_progress(void);
void watchdog_ota_end(void);
struct json_object;
void watchdog_status_json(struct json_object *out);

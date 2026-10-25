#pragma once
/* Request-local projections of authoritative module state. Never a maintained
 * second device state. Captured groups are coherent independently; the mask
 * does not promise an atomic instant spanning different subsystem locks. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
/* Both source storage and destination bounds matter (TC1 SDK name/SSID are
 * 32-byte arrays, including legal SSIDs without a trailing NUL). */
static inline void device_status_text(char *out,size_t capacity,const char *in,size_t storage)
{
    if(!capacity)return;
    size_t i=0;while(i+1<capacity && i<storage && in[i]){out[i]=in[i];i++;}out[i]=0;
}
#include "../M1/protocol.h"
#include "../M1/barometer.h"
#include "sensor_values.h"
#include "watchdog.h"
#include "httpd/http.h"
#include "../A1/filter.h"
#include "../A1/filter_stock.h"
#ifdef A1_FRESH_AIR
#include "../A1/fresh_air_model.h"
#endif

typedef enum { DEVICE_A1, DEVICE_M1, DEVICE_TC1 } device_model_t;
enum { STATUS_IDENTITY=1, STATUS_NETWORK=2, STATUS_CONTROL=4,
       STATUS_SENSOR=8, STATUS_OTA=16, STATUS_DIAGNOSTIC=32 };
typedef struct {
    char name[64], id[16];
    const char *version; /* immutable firmware literal */
} device_identity_t;
typedef struct device_network {
#ifdef PHICOMM_BEMFA
    bool mqtt_enabled, bemfa_enabled; uint8_t bemfa_status;
    unsigned bemfa_errors, bemfa_last_stage; int bemfa_last_error;
#endif
    bool wifi_connected, mqtt_connected, ap_active, mqtt_cleanup_warning;
    bool wifi_trial_active; uint8_t mqtt_status;
    char wifi_trial_status[24];
} device_network_t;
typedef struct {
    char state[32], error[128]; unsigned progress;
    bool active, reboot_pending, scan_busy;
} device_ota_t;
typedef struct {
    bool fan_on, led_enabled, led_always_on, child_lock;
    uint8_t speed, brightness, red, green, blue, applied_speed;
    bool fan_ready, touch_active, led_visible, panel_ready, config_dirty;
    bool effective_on, automatic; unsigned effective_speed, auto_limit;
    char save_error[96];
#ifdef A1_FRESH_AIR
    fresh_snapshot_t fresh;
#endif
} device_a1_control_t;
typedef struct device_m1_co2 {
    uint32_t revision;
    uint16_t interval, low, high, ppm;
    uint8_t mode, slot;
    bool supported, present, valid, showing;
} device_m1_co2_t;
typedef struct {
    bool uart_ready;
    device_m1_co2_t co2_display;
    m1_sample_t sample; /* reuse the sensor module's authoritative C format */
    struct {
        bool ready, received; uint16_t co2_ppm;
        uint32_t updated_ms, errors; int32_t last_error; char serial[13];
    } co2; /* Deliberately excludes SCD40 auxiliary temperature/humidity. */
    barometer_state_t barometer; /* Request-local copy, including live freshness. */
} device_m1_sensor_t;
typedef struct { int8_t states[6]; bool child_lock, led_enabled; } device_tc1_control_t;
typedef struct {
    device_model_t model; uint32_t captured, now_ms;
    device_identity_t identity; device_network_t network; device_ota_t ota;
    union { device_a1_control_t a1; device_tc1_control_t tc1;
        struct { uint8_t brightness_requested; } m1; } control;
    union { device_m1_sensor_t m1; feature_values_t a1; } sensor;
} device_status_t;
typedef struct {
    uint32_t now_ms; bool memory_valid; int free_memory, allocated_memory,
        free_memory_chunks, minimum_free, reset_reason;
    bool reset_valid;
} device_runtime_t;
typedef struct {
    char ssid[33], ip[16], ap_name[32], mqtt_host[128], mqtt_user[64];
    unsigned mqtt_port, mqtt_errors; char mqtt_last_error[96];
    bool web_password_set, wifi_password_set, mqtt_password_set;
} device_network_full_t;
typedef struct {
    bool signature_verified, image_verified; uint32_t key_id, image_length;
    char release_counter[21], version[32];
} device_ota_full_t;
typedef struct {
    device_network_full_t network; device_ota_full_t ota;
    device_runtime_t runtime; watchdog_status_t watchdog;
    ph_http_diagnostics http;
    union {
        struct { a1_filter_storage_t filter; a1_filter_stock_t filter_stock; uint32_t panel_errors, hardware_errors;
            int panel_last_error; uint8_t panel_raw[3]; } a1;
        struct { int timezone_minutes; char ntp_host[64], ntp_status[24], ntp_server[64];
            bool ntp_synced, ntp_screen_queued; uint32_t ntp_last_success, ntp_next_at;
            uint32_t rx_frames, unknown_frames, uart_errors, discarded_bytes;
            uint8_t last_frame[20]; uint32_t co2_samples, co2_crc_errors;
            bool co2_bus_initialized; uint16_t co2_last_command; uint8_t co2_error_phase;
        } m1;
        struct { char mask[16], gateway[16], socket_names[6][64];
            bool modern, time_synced; unsigned report_seconds, power_coefficient;
            int32_t utc_time; } tc1;
    } model;
} device_status_full_t;

struct json_object;
/* Pure encoding: no clock, state getters, locks, sockets or hardware calls. */
struct json_object *device_status_encode(const device_status_t *, const device_status_full_t *);
void device_runtime_encode(struct json_object *, const device_runtime_t *);
void a1_status_capture(device_status_t *, device_status_full_t *);
void m1_status_capture(device_status_t *, device_status_full_t *);
bool tc1_status_capture(device_status_t *, device_status_full_t *);

/* Independent text; no graph is built. Free with printbuf_free(). */
struct printbuf *device_status_wire(const device_status_t *, const device_status_full_t *, bool envelope, const char *request_id);

#pragma once
#include "mico.h"
#include "mico_system.h"
#include "protocol.h"
#include "json.h"
#include "../common/mqtt_account.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../common/generated/build_version.h"
#define A1_VERSION FIRMWARE_VERSION
#define A1_PRODUCT "PHICOMM_A1_EMW3080"
#define A1_CONFIG_MAGIC 0x41314346u
#define A1_CONFIG_VERSION 10u
#define A1_APP_OFFSET 0x75000u
#define A1_APP_CPU_ADDRESS 0x08088000u
#define A1_APP_LIMIT 0x40000u
#define A1_JSON_LIMIT 8192
#define A1_SAVE_DELAY_MS 2000u
#define A1_HARDWARE_STACK_SIZE 12288u

typedef struct {
    uint32_t magic, version, revision;
    char ssid[33], wifi_password[65];
    char mqtt_host[128], mqtt_user[64], mqtt_password[MQTT_PASSWORD_MAX + 1];
    char name[64], web_password[33];
    uint16_t mqtt_port;
    uint8_t fan_on, speed, led_enabled, led_always_on;
    uint8_t brightness, red, green, blue, child_lock;
    uint8_t mqtt_topic_mode; /* ABI retained; always MQTT_TOPIC_TMCP. */
} a1_config_t;

typedef struct {
    uint32_t config_generation, reboot_at, led_until, fan_feedback_until;
    uint32_t panel_feedback_until; /* Power-key status overlay; RAM only. */
    uint32_t mode_feedback_revision; /* Accepted explicit mode selections; RAM only. */
    uint32_t panel_errors, hardware_errors, mqtt_errors;
    char mqtt_last_error[96];
    uint32_t light_test_until;
    uint8_t light_test_slot;
    bool light_test_neighbors;
    int panel_last_error;
    bool wifi_connected, ap_active, ap_requested, mqtt_connected;
    bool fan_ready, panel_ready, led_visible, config_dirty, touch_active;
    bool reboot_pending, ota_active, scan_busy;
    bool wifi_trial_active;
    char wifi_trial_status[24];
    uint8_t applied_speed, panel_raw[3];
    uint8_t mqtt_status;
    bool mqtt_cleanup_warning;
    unsigned ota_progress, scan_count;
    bool ota_signature_verified, ota_image_verified;
    uint32_t ota_key_id, ota_image_length;
    char ota_release_counter[21], ota_version[32];
    char ip[16], ap_name[32], ota_state[16], ota_error[96], save_error[96];
    struct { char ssid[33]; int rssi; } scan[32];
} a1_state_t;

extern mico_mutex_t a1_lock;
extern char a1_id[13];
extern a1_state_t a1_state;
extern mico_Context_t *a1_context;
void a1_config_get(a1_config_t *);
bool a1_config_save(const a1_config_t *);
bool a1_config_flush(bool force);
bool a1_reset(bool full);
void a1_state_get(a1_state_t *);
bool a1_reboot_later(uint32_t ms);
bool a1_upgrade_reserve(void);
json_object *a1_json_parse(const char *, size_t);
bool a1_control_json(const char *kind, json_object *object);
enum { A1_CONTROL_FAN, A1_CONTROL_LIGHT, A1_CONTROL_LOCK };
enum { A1_SET_ON=1, A1_SET_SPEED=2, A1_SET_MODE=4, A1_SET_ENABLED=8,
       A1_SET_ALWAYS=16, A1_SET_BRIGHTNESS=32, A1_SET_COLOR=64 };
/* RAM-only request. Boolean values may be 2 (toggle); mode is 1=manual,
 * 2=auto, 3=toggle. Brightness is native 0..255. */
typedef struct {
    unsigned fields;
    uint8_t kind, on, speed, mode, enabled, always_on, brightness, red, green, blue;
} a1_control_t;
bool a1_control_apply(a1_control_t *, bool button, bool broadcast);
bool a1_control_light_percent(json_object *);
enum { A1_INPUT_BUTTON, A1_INPUT_SLIDER, A1_INPUT_NETWORK_RESET, A1_INPUT_FULL_RESET };
bool a1_input_push(unsigned kind, unsigned key, unsigned clicks, unsigned seconds, int delta);
void a1_input_poll(void);
void a1_input_clear(void);
void a1_panel_event(bool toggle, int delta);
bool a1_panel_inspect(void);
void a1_request_ap(void);
OSStatus a1_hardware_start(void);
OSStatus a1_network_start(void);
bool a1_scan_start(void);
bool a1_wifi_trial(const a1_config_t *);
OSStatus a1_mqtt_start(void);
OSStatus a1_http_start(void);
void a1_ota_status(const char *, const char *, unsigned);
bool a1_ota_begin(uint32_t);
bool a1_ota_begin_unsigned(uint32_t);
bool a1_ota_write(const uint8_t *, size_t);
bool a1_ota_finish(void);
void a1_ota_abort(const char *);
bool a1_ota_download(const char *);
json_object *a1_status_json(void);
json_object *a1_status_live_json(void);
int a1_socket_read(int, void *, size_t, unsigned);
bool a1_socket_send(int, const void *, size_t);
int a1_socket_line(int, char *, size_t);

/* Internal controller state: lock configuration before a1_lock, never reverse. */
extern mico_mutex_t a1_config_lock;
extern a1_config_t *a1_config;
void a1_control_changed(void); /* caller holds both mutexes */

#include "filter.h"
void a1_filter_tick(bool running);
void a1_filter_get(a1_filter_storage_t *);
/* Returns accepted, busy, or storage_error; parameters are prevalidated. */
const char *a1_filter_change(unsigned lifetime_hours, bool reset);
json_object *a1_filter_json(void);

#include "../common/phicomm_features.h"

bool a1_control_local(const char *, json_object *);

#include "../common/mqtt/bemfa.h"
bool a1_mqtt_config_save(const a1_config_t *,const bemfa_config_t *);

#include "filter_stock.h"
void a1_filter_observe(unsigned speed, unsigned weight);
void a1_filter_stock_get(a1_filter_stock_t *);
const char *a1_filter_model_change(bool weighted);

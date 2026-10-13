#pragma once
#include "mico.h"
#include "mico_system.h"
#include "protocol.h"
#include "json.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../common/generated/build_version.h"
#define A1_VERSION FIRMWARE_VERSION
#define A1_PRODUCT "PHICOMM_A1_EMW3080"
#define A1_CONFIG_MAGIC 0x41314346u
#define A1_CONFIG_VERSION 2u
#define A1_APP_OFFSET 0x75000u
#define A1_APP_CPU_ADDRESS 0x08088000u
#define A1_APP_LIMIT 0x40000u
#define A1_JSON_LIMIT 8192
#define A1_SAVE_DELAY_MS 2000u

typedef struct {
    uint32_t magic, version, revision;
    char ssid[33], wifi_password[65];
    char mqtt_host[128], mqtt_user[64], mqtt_password[64];
    char name[64], web_password[33];
    uint16_t mqtt_port;
    uint8_t fan_on, speed, led_enabled, led_always_on;
    uint8_t brightness, red, green, blue, child_lock;
} a1_config_t;

typedef struct {
    uint32_t config_generation, reboot_at, led_until;
    uint32_t panel_errors, hardware_errors;
    bool wifi_connected, ap_active, ap_requested, mqtt_connected;
    bool fan_ready, panel_ready, led_visible, config_dirty;
    bool reboot_pending, ota_active, scan_busy;
    uint8_t applied_speed, panel_raw[3];
    unsigned ota_progress, scan_count;
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
void a1_state_get(a1_state_t *);
bool a1_reboot_later(uint32_t ms);
bool a1_upgrade_reserve(void);
json_object *a1_json_parse(const char *, size_t);
bool a1_control_json(const char *kind, json_object *object);
void a1_panel_event(bool toggle, int delta);
void a1_request_ap(void);
OSStatus a1_hardware_start(void);
OSStatus a1_network_start(void);
bool a1_scan_start(void);
OSStatus a1_mqtt_start(void);
OSStatus a1_http_start(void);
void a1_ota_status(const char *, const char *, unsigned);
bool a1_ota_begin(uint32_t);
bool a1_ota_write(const uint8_t *, size_t);
bool a1_ota_finish(void);
void a1_ota_abort(const char *);
bool a1_ota_download(const char *);
json_object *a1_status_json(void);
int a1_socket_read(int, void *, size_t, unsigned);
bool a1_socket_send(int, const void *, size_t);
int a1_socket_line(int, char *, size_t);

/* Internal controller state: lock configuration before a1_lock, never reverse. */
extern mico_mutex_t a1_config_lock;
extern a1_config_t *a1_config;
void a1_control_changed(void); /* caller holds both mutexes */

#include "../common/phicomm_features.h"

bool a1_control_local(const char *, json_object *);

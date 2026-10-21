#pragma once
#include "scd40.h"
#include "barometer.h"
#include "mico.h"
#include "mico_system.h"
#include "protocol.h"
#include "json.h"
#include "../common/mqtt_account.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../common/generated/build_version.h"
#define M1_VERSION FIRMWARE_VERSION
#define M1_PRODUCT "PHICOMM_M1_EMW3080"
#define M1_CONFIG_MAGIC 0x4d314346u
#define M1_CONFIG_VERSION 5u
#define M1_APP_OFFSET 0x75000u
#define M1_APP_CPU_ADDRESS 0x08088000u
#define M1_APP_LIMIT 0x40000u
#define M1_JSON_LIMIT 8192

typedef struct {
    uint32_t magic, version, revision;
    char ssid[33], wifi_password[65];
    char mqtt_host[128], mqtt_user[64], mqtt_password[MQTT_PASSWORD_MAX + 1];
    char name[64], web_password[33];
    uint16_t mqtt_port;
    uint8_t brightness, last_on;
    int16_t timezone_minutes;
    uint8_t mqtt_topic_mode; /* ABI retained; always MQTT_TOPIC_TMCP. */
} m1_config_t;

typedef struct {
    m1_sample_t sample;
    scd40_state_t scd40;
    barometer_state_t barometer;
    uint8_t last_frame[20], brightness_requested;
    uint32_t rx_frames, unknown_frames, uart_errors, discarded_bytes;
    uint32_t config_generation, reboot_at;
    uint32_t mqtt_errors;
    char mqtt_last_error[96];
    bool ntp_synced, ntp_screen_queued;
    uint32_t ntp_last_success, ntp_next_at;
    char ntp_server[64], ntp_status[24];
    bool wifi_connected, ap_active, mqtt_connected, uart_ready;
    bool reboot_pending, ota_active, scan_busy;
    bool wifi_trial_active;
    char wifi_trial_status[24];
    uint8_t mqtt_status;
    bool mqtt_cleanup_warning;
    unsigned ota_progress, scan_count;
    bool ota_signature_verified, ota_image_verified;
    uint32_t ota_key_id, ota_image_length;
    char ota_release_counter[21], ota_version[32];
    char ip[16], ap_name[32], ota_state[16], ota_error[96];
    struct { char ssid[33]; int rssi; } scan[32];
} m1_state_t;

extern mico_mutex_t m1_lock;
extern char m1_id[13];
extern m1_state_t m1_state;
extern mico_Context_t *m1_context;

void m1_config_get(m1_config_t *);
void m1_ntp_host_get(char out[64]);
bool m1_config_save(const m1_config_t *);
bool m1_reset(bool full);
void m1_state_get(m1_state_t *);
bool m1_reboot_later(uint32_t ms);
OSStatus m1_uart_start(void);
bool m1_set_brightness(unsigned);
bool m1_uart_clock(unsigned, unsigned, unsigned, unsigned, unsigned, unsigned);
OSStatus m1_network_start(void);
bool m1_scan_start(void);
bool m1_wifi_trial(const m1_config_t *);
OSStatus m1_mqtt_start(void);
OSStatus m1_http_start(void);
void m1_ota_status(const char *state, const char *error, unsigned progress);
bool m1_ota_begin(uint32_t length);
bool m1_ota_begin_unsigned(uint32_t length);
bool m1_ota_write(const uint8_t *, size_t);
bool m1_ota_finish(void);
void m1_ota_abort(const char *);
bool m1_ota_download(const char *url);
json_object *m1_status_json(void);
json_object *m1_status_live_json(void);

/* Socket helpers use a bounded wait even if the peer never sends a byte. */
int m1_socket_read(int fd, void *buf, size_t length, unsigned timeout_ms);
bool m1_socket_send(int fd, const void *buf, size_t length);
int m1_socket_line(int fd, char *buf, size_t capacity);


#include "../common/phicomm_features.h"

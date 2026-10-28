#pragma once
#include "scd40.h"
#include "barometer.h"
#include "display_source.h"
#include "mico.h"
#include "mico_system.h"
#include "protocol.h"
#include "json.h"
#include "../common/phicomm_features.h"
#include "../common/mqtt_account.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../common/generated/build_version.h"
#define M1_VERSION FIRMWARE_VERSION
#define M1_PRODUCT "PHICOMM_M1_EMW3080"
#define M1_CONFIG_MAGIC 0x4d314346u
#define M1_CONFIG_VERSION 11u
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

/* Fits the retained 20-byte v6 controller extension; not MCU readback. */
#define M1_ALARM_MAGIC 0x4d31414cu
#define M1_ALARM_SCHEMA 1u
typedef struct {
    uint32_t magic, revision;
    uint16_t hcho[2], pm25[2];
    /* reserved[0]: schema; reserved[1]: HCHO/PM2.5 buzzer mask; [2]: zero. */
    uint8_t configured, reserved[3];
} m1_alarm_config_t;
typedef char m1_alarm_size_check[sizeof(m1_alarm_config_t) == 20 ? 1 : -1];
typedef struct {
    uint32_t magic, revision;
    uint16_t interval, low, high;
    uint8_t mode, slot;
} m1_co2_config_t;
#define M1_CO2_MAGIC 0x4d314332u
#define M1_CO2_SOURCE_MAGIC 0x4d314353u
typedef struct {
    uint32_t magic, revision;
    char device_id[13];
    uint8_t external, double_click, reserved[2];
} m1_co2_source_config_t;
void m1_co2_source_config_get(m1_co2_source_config_t *);
const char *m1_co2_source_config_save(const m1_co2_source_config_t *);
static inline bool m1_co2_source_config_valid(const m1_co2_source_config_t *c)
{
    if(c->magic!=M1_CO2_SOURCE_MAGIC || c->revision>INT32_MAX ||
       c->external>1 || c->double_click>1 || c->device_id[12] || c->reserved[0] || c->reserved[1])return false;
    if(!c->external){for(unsigned i=0;i<13;i++)if(c->device_id[i])return false;return true;}
    for(unsigned i=0;i<12;i++){
        char x=c->device_id[i];if(!((x>='0'&&x<='9')||(x>='a'&&x<='f')))return false;
    }
    return true;
}
void m1_co2_source_reset(void);
void m1_co2_source_sample(const char *,const feature_sample_t *);
void m1_co2_source_invalidate(const char *);
bool m1_co2_source_value(uint16_t *,uint16_t *);
#include "../common/typed_wire.h"
bool m1_co2_source_mp(const char *,const ph_value *,ph_writer *,const char **);
bool m1_button_rx(const uint8_t [20]);
void m1_display_config_get(m1_display_config_t *);
void m1_display_switch_get(m1_display_switch_config_t *);
bool m1_display_lane_get(unsigned,m1_display_lane_config_t *,m1_display_remote_t *,uint32_t *);
const char *m1_display_config_save(const m1_display_config_t *);
void m1_display_source_reset(void);
void m1_display_source_mqtt_reset(void);
void m1_display_source_metadata(const char *,const ph_value *);
void m1_display_source_sample(const char *,const ph_value *);
void m1_display_source_offline(const char *,const char *);
void m1_display_source_value(const m1_display_config_t *,unsigned,m1_display_value_t *);
void m1_display_data_store(unsigned,uint32_t,float,uint8_t,int);
void m1_display_frame_store(unsigned,uint32_t,unsigned,unsigned,unsigned,unsigned);
const char *m1_display_data_save(unsigned,uint32_t,float,unsigned,unsigned,int);
const char *m1_display_frame_save(unsigned,uint32_t,unsigned,unsigned,unsigned,unsigned);
void m1_display_profile_write(ph_writer *);
bool m1_display_mp(const char *,const ph_value *,ph_writer *,const char **);
void m1_display_readback(m1_display_readback_t [M1_DISPLAY_LANES],bool *);

typedef struct {
    char mcu_version[17];
    bool mcu_version_received, buzzer_requested, wake_alarm_active;
    bool mcu_tones, mcu_button_events, mcu_display_source;
    uint8_t mcu_display_schema; /* Independent dual-lane extension, not scalar schema 1. */
    uint8_t mcu_display_format_schema; /* Read-only OP26 discovery; OP00 remains schema2. */
    bool mcu_music_present;
    bool mcu_sensor_details;
    uint8_t wake_alarm_tone;
    uint32_t wake_alarm_started, wake_alarm_until;
    uint32_t mcu_version_updated_ms, buzzer_until;
    bool alarm_uart_sent;
    uint32_t alarm_sent_revision;
    uint8_t mcu_alarm_schema, alarm_buzzer_mask;
    bool alarm_buzzer_received, alarm_buzzer_active;
    uint32_t alarm_buzzer_revision, alarm_buzzer_updated_ms;
    bool mcu_updating, mcu_extended;
    uint16_t mcu_caps;
    uint8_t mcu_protocol;
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

bool m1_uart_version_query(void);
bool m1_uart_buzzer(bool enabled, unsigned duration_ms);
json_object *m1_mcu_json(void);
void m1_alarm_get(m1_alarm_config_t *);
/* buzzer: -1 preserves the saved selection; 0/1 explicitly selects it. */
const char *m1_alarm_set(bool pm25, uint16_t low, uint16_t high, uint32_t revision, int buzzer);
json_object *m1_alarm_json(void);
void m1_co2_config_get(m1_co2_config_t *);
const char *m1_co2_config_save(const m1_co2_config_t *);
struct device_m1_co2;
void m1_co2_capture(struct device_m1_co2 *);
void m1_details_snapshot(m1_details_t *out);
bool m1_mcu_exchange(const uint8_t *,size_t,uint8_t reply[20]);
void m1_mcu_tick(void);
bool m1_mcu_rx(const uint8_t frame[20]);
bool m1_wake_alarm_start(unsigned duration_seconds);
bool m1_tone_supported(void);
bool m1_music_supported(void);
bool m1_wake_alarm_tone_start(unsigned duration_seconds,unsigned tone);
void m1_wake_alarm_stop(void);
json_object *m1_wake_alarm_json(void);

#include "../common/i18n.h"
uint8_t device_language_get(void);
bool m1_device_config_save(const m1_config_t *, uint8_t language);

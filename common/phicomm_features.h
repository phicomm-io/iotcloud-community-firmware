#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FEATURE_MAX_PEERS 8
#define FEATURE_MAX_EVENTS 8
#define FEATURE_MAX_SLEEP 4
#define FEATURE_WIRE_MAX 1200
#include "mqtt_account.h"
#define FEATURE_TOPIC_MAX MQTT_TOPIC_MAX
#define FEATURE_PORT 11288
#define FEATURE_MAGIC 0x42494e32u
#define FEATURE_STALE_MS 15000u

enum { FEATURE_M1 = 1, FEATURE_A1 = 2, FEATURE_GROUP = 3 };
enum {
  STAT_MEAN,
  STAT_MAX,
  STAT_MIN,
  STAT_MEDIAN,
  STAT_TRIMMED,
  STAT_FAILBACK
};
enum {
  FIELD_TEMP,
  FIELD_HUMIDITY,
  FIELD_PM25,
  FIELD_HCHO,
  FIELD_QUALITY,
  FIELD_FAN
};
enum { EFFECT_FAN, EFFECT_SOLID, EFFECT_BREATHE, EFFECT_RAINBOW, EFFECT_DATA };
enum { DISPLAY_VALUE, DISPLAY_SUITABILITY, DISPLAY_BOTH };

typedef struct {
  char id[13], ip[16], group[33];
  uint8_t token[16];
  uint8_t kind, confirmed, force_mqtt, recovery, sensor_count;
  uint16_t timeout_ms, recovery_seconds;
} feature_peer_t;
typedef struct {
  char id[13], ip[16];
} feature_member_t;
typedef struct {
  uint8_t enabled, mode, metrics, statistic, on_enabled, off_enabled, on_all,
      off_all;
  uint8_t fixed_speed, min_speed, max_speed, reserved, on_metrics, off_metrics;
  uint16_t debounce_seconds, min_switch_seconds;
  float on_pm, off_pm, on_hcho, off_hcho, target_pm, target_hcho;
  float low_pm, high_pm, low_hcho, high_hcho, kp, ki, kd;
} feature_linkage_t;
typedef struct {
  uint8_t source, display, colors[3][3], reserved;
  float minimum, maximum, suitable_low, suitable_high;
} feature_segment_t;
typedef struct {
  uint8_t effect, segments, follow_fan, reserved;
  float quality_pm, quality_hcho;
  feature_segment_t segment[4];
} feature_light_t;
typedef struct {
  uint8_t days, on, speed, reserved;
  uint16_t minute;
} feature_event_t;
typedef struct {
  uint8_t days, fan, light, max_speed, brightness, block_start;
  uint16_t start, end;
} feature_sleep_t;
typedef struct {
  uint32_t magic, revision;
  char group[33], ntp_host[64];
  uint8_t group_token[16], peer_count, member_count, event_count, sleep_count;
  int16_t timezone_minutes;
  uint8_t scheduled_off, scheduled_valid, scheduled_speed, group_force_mqtt,
      group_recovery, reserved;
  uint16_t group_timeout_ms, group_recovery_seconds;
  uint64_t last_event;
  feature_peer_t peers[8];
  feature_member_t members[8];
  feature_linkage_t linkage;
  feature_light_t light;
  feature_event_t events[8];
  feature_sleep_t sleeps[4];
} feature_config_t;
typedef struct {
  float value[4];
  uint32_t sequence, boot, received_ms, age_ms;
  bool valid;
} feature_sample_t;
typedef struct {
  float integral, previous, derivative;
  uint32_t last_ms, condition_since, switch_ms;
  int condition;
  bool initialized;
} feature_pid_t;
typedef struct {
  bool fan_sleep, light_sleep, block_start;
  uint8_t max_speed, brightness;
} feature_sleep_state_t;

void feature_defaults(feature_config_t *);
void feature_light_layout(feature_light_t *, unsigned);
bool feature_mac(const char *, char out[13]);
void feature_colon_mac(const char *, char out[18]);
bool feature_group_valid(const char *);
void feature_root(const char *, const char *, const char *, unsigned, char *,
                  size_t);
bool feature_sample_fresh(const feature_sample_t *, uint32_t);
unsigned feature_aggregate(const feature_sample_t *, unsigned, uint32_t,
                           unsigned, float out[4], unsigned *effective);
bool feature_control(const feature_linkage_t *, feature_pid_t *, const float[4],
                     uint32_t, bool, unsigned, bool *, unsigned *);
feature_sleep_state_t feature_sleep_at(const feature_config_t *, unsigned day,
                                       unsigned minute);
int feature_latest_event(const feature_config_t *, uint64_t utc, uint64_t after,
                         uint64_t *stamp);
void feature_render(const feature_light_t *, const float[4], const bool[4],
                    unsigned, unsigned, bool, uint32_t, uint32_t out[32]);

#ifndef FEATURE_HOST
#include "json.h"
#include "mico.h"
void feature_config_get(feature_config_t *);
bool feature_config_save(const feature_config_t *);
OSStatus feature_start(void);
json_object *feature_json(const char *domain);
/* Returns HTTP status; response ownership passes to caller. */
int feature_set(const char *domain, json_object *, json_object **response);
bool feature_mqtt_input(const char *, const void *, size_t, bool);
int feature_mqtt_service(int (*publish)(const char *, const char *, bool),
                          bool);
void feature_mqtt_session(const mqtt_account_t *);
int feature_mqtt_cleanup(int (*publish)(const char *, const char *, bool));
unsigned feature_mqtt_filters(char filters[12][FEATURE_TOPIC_MAX]);
bool feature_bound(void);
bool feature_is_coordinator(void);
void feature_local_sample(const feature_sample_t *);
void feature_effective_fan(bool requested_on, unsigned requested_speed, bool *,
                           unsigned *);
void feature_light_frame(uint32_t out[32], unsigned brightness, bool enabled,
                         uint32_t color);
void feature_manual_fan(bool on);
bool feature_group_control(const char *kind, json_object *);
void feature_clock_valid(void);
void feature_clock_tick(void);
#endif

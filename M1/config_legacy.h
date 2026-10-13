#pragma once
/* Frozen on-flash v1/v2 ABI. Never change these fields. */
typedef struct {
    uint32_t magic, version, revision;
    char ssid[33], wifi_password[65];
    char mqtt_host[128], mqtt_user[64], mqtt_password[64];
    char name[64], web_password[33];
    uint16_t mqtt_port;
    uint8_t brightness, last_on;
    int16_t timezone_minutes;
} m1_config_v2_t;
typedef struct { m1_config_v2_t base; feature_config_t features; } m1_storage_v2_t;

#include <stddef.h>
static inline void m1_migrate_v2(m1_config_t *next, feature_config_t *features, const m1_storage_v2_t *old)
{
    memset(next,0,sizeof(*next));
    memcpy(next, &old->base, offsetof(m1_config_v2_t, mqtt_password));
    memcpy(next->mqtt_password, old->base.mqtt_password, sizeof(old->base.mqtt_password));
    memcpy(next->name, old->base.name, sizeof(old->base.name));
    memcpy(next->web_password, old->base.web_password, sizeof(old->base.web_password));
    next->mqtt_port = old->base.mqtt_port;
    next->brightness = old->base.brightness;
    next->last_on = old->base.last_on;
    next->timezone_minutes = old->base.timezone_minutes;
    next->version = M1_CONFIG_VERSION;
    next->mqtt_topic_mode = MQTT_TOPIC_LEGACY;
    if (old->base.version == 2 && old->features.magic == FEATURE_MAGIC) *features = old->features;
    else feature_defaults(features);
}

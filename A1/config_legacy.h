#pragma once
/* Frozen on-flash v1/v2 ABI. Never change these fields. */
typedef struct {
    uint32_t magic, version, revision;
    char ssid[33], wifi_password[65];
    char mqtt_host[128], mqtt_user[64], mqtt_password[64];
    char name[64], web_password[33];
    uint16_t mqtt_port;
    uint8_t fan_on, speed, led_enabled, led_always_on;
    uint8_t brightness, red, green, blue, child_lock;
} a1_config_v2_t;
typedef struct { a1_config_v2_t base; feature_config_storage_t features; } a1_storage_v2_t;

#include <stddef.h>
static inline void a1_migrate_v2(a1_config_t *next, feature_config_t *features, const a1_storage_v2_t *old)
{
    memset(next,0,sizeof(*next));
    memcpy(next, &old->base, offsetof(a1_config_v2_t, mqtt_password));
    memcpy(next->mqtt_password, old->base.mqtt_password, sizeof(old->base.mqtt_password));
    memcpy(next->name, old->base.name, sizeof(old->base.name));
    memcpy(next->web_password, old->base.web_password, sizeof(old->base.web_password));
    next->mqtt_port = old->base.mqtt_port;
    next->fan_on = old->base.fan_on;
    next->speed = old->base.speed;
    next->led_enabled = old->base.led_enabled;
    next->led_always_on = old->base.led_always_on;
    next->brightness = old->base.brightness;
    next->red = old->base.red;
    next->green = old->base.green;
    next->blue = old->base.blue;
    next->child_lock = old->base.child_lock;
    next->version = A1_CONFIG_VERSION;
    next->mqtt_topic_mode = MQTT_TOPIC_TMCP;
    feature_defaults(features);
    if (old->base.version == 2 && old->features.magic == FEATURE_MAGIC) memcpy(features, &old->features, sizeof(old->features));
}

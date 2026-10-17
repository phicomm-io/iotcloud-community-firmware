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
typedef struct { m1_config_v2_t base; feature_config_storage_t features; } m1_storage_v2_t;

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
    next->mqtt_topic_mode = MQTT_TOPIC_TMCP;
    feature_defaults(features);
    if (old->base.version == 2 && old->features.magic == FEATURE_MAGIC) memcpy(features, &old->features, sizeof(old->features));
}

/* v3 -> v4 keeps the base ABI. Only local response permissions survive from
 * the former bidirectional linkage configuration; sessions are RAM-only. */
static inline void m1_migrate_response_permissions(m1_config_t *base, feature_config_t *f)
{
    if (f->magic != FEATURE_MAGIC) feature_defaults(f);
    unsigned count = 0, previous = f->peer_count > 8 ? 8 : f->peer_count;
    for (unsigned i = 0; i < previous; i++) {
        feature_peer_t peer = f->peers[i];
        bool valid = peer.kind == FEATURE_A1 || peer.kind == FEATURE_GROUP;
        for (unsigned k = 0; k < 12; k++) {
            char c = peer.id[k];
            if (c >= 'A' && c <= 'F') peer.id[k] += 'a' - 'A';
            else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) valid = false;
        }
        valid = valid && peer.id[12] == 0;
        for (unsigned j = 0; j < count; j++) if (!strcmp(f->peers[j].id,peer.id)) valid = false;
        if (valid) {
            memset(&f->peers[count],0,sizeof(f->peers[count]));
            strcpy(f->peers[count].id,peer.id);f->peers[count++].kind=FEATURE_A1;
        }
    }
    memset(f->peers+count,0,(8-count)*sizeof(f->peers[0]));f->peer_count=count;
    f->group[0]=0;f->member_count=0;memset(f->members,0,sizeof(f->members));
    memset(f->group_token,0,sizeof(f->group_token));
    memset(&f->linkage,0,sizeof(f->linkage));memset(&f->light,0,sizeof(f->light));
    f->timezone_minutes=base->timezone_minutes;
    base->version=M1_CONFIG_VERSION;
}

#pragma once
#include "m1.h"
#include "../common/automation.h"
#include "../common/phicomm_features.h"
#include <stddef.h>

/* Frozen v8 payload: backups include sizeof, including its trailing padding.
 * The v9 display reuses that padding, so its offset is not the v8 size. */
typedef struct {
    m1_config_t base;
    feature_config_storage_t features;
    automation_config_t automation;
    uint8_t reserved_controller_config[20];
    m1_co2_config_t co2;
    m1_co2_source_config_t co2_source;
} m1_storage_v8_t;

/* Frozen v9 display prefix. Presentation records are v10-only; old backup
 * payloads must never be read using sizeof the current display structure. */
typedef struct {
    uint32_t magic, revision;
    uint8_t configured, selected, double_click, reserved;
    m1_display_lane_config_t lanes[M1_DISPLAY_LANES];
} m1_display_config_v9_t;
typedef struct {
    m1_config_t base;
    feature_config_storage_t features;
    automation_config_t automation;
    uint8_t reserved_controller_config[20];
    m1_co2_config_t co2;
    m1_co2_source_config_t co2_source;
    m1_display_config_v9_t display;
} m1_storage_v9_t;

/* Retain every v9 offset and append only the two presentation records. */
typedef struct {
    m1_config_t base;
    feature_config_storage_t features;
    automation_config_t automation;
    uint8_t reserved_controller_config[20];
    m1_co2_config_t co2;
    m1_co2_source_config_t co2_source;
    m1_display_config_t display;
} app_storage_t;

typedef char m1_display_v9_size_check[sizeof(m1_display_config_v9_t)==76 ? 1 : -1];
typedef char m1_display_v10_prefix_check[offsetof(m1_display_config_t,style)==sizeof(m1_display_config_v9_t) ? 1 : -1];
typedef char m1_storage_v10_prefix_check[offsetof(app_storage_t,display)==offsetof(m1_storage_v9_t,display) ? 1 : -1];

/* Copy only the frozen bytes into initialized current storage. A v9 record
 * cannot opt into the formatted profile, even if its prefix otherwise passes
 * the newer validator. The on-flash tail after this prefix may be arbitrary. */
static inline bool m1_display_v9_valid(const void *legacy)
{
    m1_display_config_t next;
    m1_display_defaults(&next);
    memcpy(&next,legacy,sizeof(m1_display_config_v9_t));
    return !next.reserved && m1_display_config_valid(&next);
}

static inline void m1_display_upgrade_v9(m1_display_config_t *display)
{
    memset(display->style,0,sizeof(display->style));
}

#pragma once
#include <stddef.h>
#include "../common/local_auth.h"
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

/* Frozen v10 includes presentation, but not generic field bindings. */
typedef struct {
    m1_display_config_v9_t prefix;
    m1_display_style_t style[M1_DISPLAY_LANES];
} m1_display_config_v10_t;
typedef struct {
    m1_config_t base;
    feature_config_storage_t features;
    automation_config_t automation;
    uint8_t reserved_controller_config[20];
    m1_co2_config_t co2;
    m1_co2_source_config_t co2_source;
    m1_display_config_v10_t display;
} m1_storage_v10_t;

/* Retain both frozen prefixes and append the two generic field bindings. */
typedef struct {
    m1_config_t base;
    feature_config_storage_t features;
    automation_config_t automation;
    uint8_t reserved_controller_config[20];
    m1_co2_config_t co2;
    m1_co2_source_config_t co2_source;
    m1_display_config_t display;
} m1_storage_v11_t;

/* Preserve the complete old object, including trailing padding. New records
 * begin after its sizeof; the anonymous view retains existing member access. */
typedef struct {
    union {
        m1_storage_v11_t frozen_prefix;
        struct {
            m1_config_t base;
            feature_config_storage_t features;
            automation_config_t automation;
            uint8_t reserved_controller_config[20];
            m1_co2_config_t co2;
            m1_co2_source_config_t co2_source;
            m1_display_config_t display;
        };
    };
    ph_auth_store local_auth;
} app_storage_t;
typedef char m1_auth_trailer_offset_check[
    offsetof(app_storage_t,local_auth)==sizeof(m1_storage_v11_t) ? 1 : -1];

typedef char m1_display_v9_size_check[sizeof(m1_display_config_v9_t)==76 ? 1 : -1];
typedef char m1_display_v10_prefix_check[offsetof(m1_display_config_t,style)==sizeof(m1_display_config_v9_t) ? 1 : -1];
typedef char m1_display_v11_prefix_check[offsetof(m1_display_config_t,remote)==sizeof(m1_display_config_v10_t) ? 1 : -1];
typedef char m1_storage_v10_prefix_check[offsetof(app_storage_t,display)==offsetof(m1_storage_v9_t,display) ? 1 : -1];

/* Copy only the frozen bytes into initialized current storage. A v9 record
 * cannot opt into the formatted profile, even if its prefix otherwise passes
 * the newer validator. The on-flash tail after this prefix may be arbitrary. */
static inline bool m1_display_legacy_valid(const void *legacy,size_t length)
{
    m1_display_config_t next;
    m1_display_defaults(&next);
    memcpy(&next,legacy,length);
    if(length==sizeof(m1_display_config_v9_t) && next.reserved)return false;
    for(unsigned i=0;i<M1_DISPLAY_LANES;i++)
        if(next.lanes[i].source==M1_DISPLAY_MQTT && next.lanes[i].metric==M1_DISPLAY_CUSTOM)return false;
    m1_display_upgrade_remote(&next);
    return m1_display_config_valid(&next);
}
static inline bool m1_display_v9_valid(const void *legacy)
{return m1_display_legacy_valid(legacy,sizeof(m1_display_config_v9_t));}
static inline bool m1_display_v10_valid(const void *legacy)
{return m1_display_legacy_valid(legacy,sizeof(m1_display_config_v10_t));}

static inline void m1_display_upgrade_v9(m1_display_config_t *display)
{
    memset(display->style,0,sizeof(display->style));
}

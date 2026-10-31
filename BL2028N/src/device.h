/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "../protocol/typed_wire.h"
#include "meter.h"
#define PH_MODEL "s30-v1b"
#define PH_INPUT_MAX 1200u
#define PH_OUTPUT_MAX 4096u
#define PH_STATUS_CACHE_MAX 2048u
#define PH_STORE_MAGIC UINT32_C(0x334c4250)
typedef struct {
    uint32_t magic, revision;
    uint8_t child_lock, indicator, power_on, calibrated;
    double coefficients[3], energy_kwh;
    ph_auth_store auth;
    uint32_t report_seconds;
    char name[64], socket_name[64];
} ph_settings;
typedef struct {
    void *user;
    uint32_t (*now)(void *);
    bool (*save)(void *, const ph_settings *);
    void (*relay)(void *, bool);
    void (*reboot)(void *);
    void (*network_info)(void *, ph_writer *);
    const char *(*platform_action)(void *, const char *, const ph_value *, ph_writer *, bool);
    void (*diagnostics)(void *, ph_writer *);
    uint32_t (*network_stamp)(void *);
} ph_device_ops;
typedef struct {
    char id[65], action[40];
    uint8_t domain;
    unsigned char params[512], data[512];
    size_t params_len, data_len;
    const char *code;
} ph_receipt;
typedef struct {
    ph_settings settings;
    ph_device_ops ops;
    char mac[13], version[64];
    uint32_t boot, sequence, local_at;
    bool relay, local_hold, maintenance, cache_suspended;
    ph_meter meter;
    ph_receipt receipts[8];
    unsigned next_receipt;
    unsigned char *status_cache;
    size_t status_size;
    uint32_t cache_sequence,cache_frames,cache_network,cache_builds;
    unsigned cache_fields;
} ph_device;
bool ph_binary_format(ph_wire_format);
void ph_settings_default(ph_settings *);
bool ph_settings_valid(const ph_settings *);
bool ph_settings_migrate_meter_defaults(ph_settings *);
bool ph_device_init(ph_device *, const ph_device_ops *, const ph_settings *, const char *, const char *, uint32_t);
/* Caller serializes state access; all views and output buffers are borrowed. */
/* Release the optional cache before reinitializing or disposing an initialized device. */
void ph_status_cache_release(ph_device *);
/* Independent of OTA maintenance; caller holds the product state lock. */
void ph_status_cache_suspend(ph_device *, bool);
void ph_status_cache_budget(ph_device *, size_t available, size_t reserve);
void ph_device_set_maintenance(ph_device *, bool);
size_t ph_command(ph_device *, const void *, size_t, ph_wire_format, ph_wire_format,
                  bool authorized, bool admin, void *, size_t);
/* LAN v4 only: typed views expire on return. receipt_key is the adapter's
 * peer/session hash; response IDs always retain request_id. Caller holds the
 * product/auth lock and owns the reserved response before admission. */
size_t ph_command_context(ph_device *,const char *action,const ph_value *params,
                          const char *request_id,const char *receipt_key,
                          const ph_auth_context *,ph_wire_format,void *,size_t,bool *admitted);
size_t ph_query(ph_device *, const char *, ph_wire_format, void *, size_t);
void ph_status(ph_device *, ph_writer *);
void ph_capabilities(ph_device *, ph_writer *);
void ph_diagnostics(ph_device *, ph_writer *);
void ph_diag_uint(ph_writer *, unsigned, uint32_t);
void ph_diag_int(ph_writer *, unsigned, int32_t);
void phw_cached_fields(ph_writer *, const void *, size_t);
bool ph_local_toggle(ph_device *);
bool ph_topic_root(const char *username, const char *mac, char *, size_t);

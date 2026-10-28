/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "../typed_wire.h"
#define BEMFA_MAGIC 0x424d5131u
#define BEMFA_TOPIC_BYTES 64
#define BEMFA_TOPICS 6
/* Append-only storage extension. Never change the retained product base ABI. */
typedef struct {
    uint32_t magic, revision;
    uint16_t version;
    uint8_t mqtt_enabled, enabled;
    char app_id[65], secret_key[129];
    char topics[BEMFA_TOPICS][BEMFA_TOPIC_BYTES];
    uint8_t language; /* Former zero-filled tail padding; TC1 device language. */
} bemfa_config_t;
typedef char bemfa_storage_abi[sizeof(bemfa_config_t)==592 && offsetof(bemfa_config_t,language)==590?1:-1];
void bemfa_defaults(bemfa_config_t *, const char *model, const char *mac);
bool bemfa_config_valid(const bemfa_config_t *, bool tc1);
bool bemfa_config_values_mp(bemfa_config_t *,const ph_value *,bool tc1);
void bemfa_config_mp_fields(ph_writer *);
void bemfa_config_write_mp(ph_writer *);
bool bemfa_config_set_mp(const ph_value *,const char **code);
struct device_network;
void bemfa_status_capture(struct device_network *);
/* Product implementations serialize configuration access with their own lock. */
void bemfa_config_get(bemfa_config_t *);
bool bemfa_config_save(const bemfa_config_t *);
bool bemfa_primary_enabled(void);
bool bemfa_network_ready(void);
const char *bemfa_device_id(void);
/* Called between HTTP server steps, never inside a Paho callback. */
void bemfa_poll(void *owner);
void bemfa_stop(void);
/* Called between HTTP server steps on its existing business-capable stack. */
void bemfa_dispatch(void *owner);
/* Pure strict protocol helpers, shared with host tests. */
typedef struct { unsigned kind, index, value; bool on, has_value; } bemfa_command_t;
enum { BEMFA_SOCKET, BEMFA_FAN, BEMFA_LIGHT };
bool bemfa_parse(unsigned kind, const void *, size_t, bemfa_command_t *);
unsigned bemfa_fan_gear(unsigned percentage);
unsigned bemfa_gear_speed(unsigned gear);

#pragma once
#include "../common/phicomm_features.h"
#include "fresh_air_model.h"
#include "../common/typed_wire.h"
#define FRESH_MAGIC 0x46524131u
#define FRESH_SCHEMA 2u
typedef struct {
    uint32_t magic, schema, revision;
    uint8_t on, speed, automatic, fallback_speed;
    fresh_mapping_t mapping;
    feature_linkage_t linkage;
    feature_co2_config_t co2;
} fresh_config_t;
void fresh_defaults(fresh_config_t *);
#ifdef A1_FRESH_AIR
void fresh_config_get(fresh_config_t *);
const char *fresh_config_save(const fresh_config_t *);
OSStatus fresh_boot(void);
void fresh_shutdown(void);
void fresh_init(void);
void fresh_prepare(bool master, unsigned desired, unsigned applied, uint32_t now, unsigned *next);
void fresh_finish(unsigned primary, uint32_t now);
void fresh_snapshot(fresh_snapshot_t *);
bool fresh_mp(const char *,const ph_value *,ph_writer *,const char **);
bool feature_fresh_values(unsigned statistic, unsigned metrics, float values[4], float *co2);
unsigned feature_primary_ceiling(void);
void feature_fresh_fields_mp(const fresh_config_t *,ph_writer *);
bool feature_fresh_decode_mp(fresh_config_t *,const ph_value *);
#endif

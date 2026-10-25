#pragma once
#include "m1.h"
#include "../common/automation.h"
#include "../common/phicomm_features.h"

/* Retain every v7 offset, including the former controller extension. */
typedef struct {
    m1_config_t base;
    feature_config_storage_t features;
    automation_config_t automation;
    uint8_t reserved_controller_config[20];
    m1_co2_config_t co2;
    m1_co2_source_config_t co2_source;
} app_storage_t;

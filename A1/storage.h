#pragma once
#include "a1.h"
#include "../common/automation.h"
#include "filter.h"

/* Append extensions only: the v3/v4 base/features/automation offsets are ABI. */
typedef struct {
    a1_config_t base;
    feature_config_t features;
    automation_config_t automation;
    a1_filter_storage_t filter;
} app_storage_t;

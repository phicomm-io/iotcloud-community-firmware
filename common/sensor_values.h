#pragma once
#include <stdbool.h>
/* Request-local projection of the values used by A1 controls and lighting. */
typedef struct {
    float value[4], co2;
    unsigned sample_count, co2_sample_count, configured_count, statistic;
    bool valid;
} feature_values_t;

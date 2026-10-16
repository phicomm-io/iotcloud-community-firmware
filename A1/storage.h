#pragma once
#include "a1.h"
#include "filter.h"
#include "../common/automation.h"

typedef struct {
    a1_config_t base;
    feature_config_t features;
    automation_config_t automation;
    a1_filter_config_t filter;
} app_storage_t;

/* v4 ends after the two original key maps. Preserve that entire prefix. */
static inline bool a1_storage_upgrade_v4(app_storage_t *c)
{
    if (c->base.version != 4) return false;
    memset(c->automation.buttons[2], 0, sizeof(c->automation.buttons[2]));
    c->filter = (a1_filter_config_t){ A1_FILTER_MAGIC, 0, A1_FILTER_DEFAULT_HOURS };
    c->base.version = A1_CONFIG_VERSION;
    return true;
}

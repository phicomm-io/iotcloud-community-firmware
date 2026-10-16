#pragma once
#include "a1.h"
#include "../common/automation.h"
#include "filter.h"

/* Append extensions only: the v3/v4 base/features/automation offsets are ABI. */
typedef struct {
    a1_config_t base;
    feature_config_t features;
    automation_storage_t automation;
    a1_filter_storage_t filter;
    automation_action_t combination_buttons[34];
} app_storage_t;

/* These copies intentionally skip the filter between the retained key maps
 * and the appended combination map. Caller holds the configuration mutex. */
static inline void a1_storage_automation_get(const app_storage_t *s, automation_config_t *out)
{
    memcpy(out, &s->automation, sizeof(s->automation));
    memcpy(out->buttons[2], s->combination_buttons, sizeof(s->combination_buttons));
}
static inline void a1_storage_automation_put(app_storage_t *s, const automation_config_t *in)
{
    memcpy(&s->automation, in, sizeof(s->automation));
    memcpy(s->combination_buttons, in->buttons[2], sizeof(s->combination_buttons));
}
static inline void a1_storage_automation_defaults(app_storage_t *s)
{
    automation_config_t next;
    automation_defaults(&next);
    a1_storage_automation_put(s, &next);
}

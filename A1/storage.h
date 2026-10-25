#pragma once
#include "a1.h"
#include "fresh_air.h"
#include "../common/automation.h"
#include "filter.h"
#include "filter_stock.h"
#include "../common/mqtt/bemfa.h"

/* Append extensions only: the v3/v4 base/features/automation offsets are ABI. */
typedef struct {
    a1_config_t base;
    feature_config_storage_t features;
    automation_storage_t automation;
    a1_filter_storage_t filter;
    automation_action_t combination_buttons[34];
    feature_auto_config_t automatic;
    feature_co2_config_t co2;
    bemfa_config_t bemfa;
    fresh_config_t fresh_air; /* Tagged append-only extension; retained by ordinary A1. */
    a1_filter_stock_t filter_stock; /* v10: after all v9 fields, including fresh-air. */
} app_storage_t;

/* Only replace the previous default power toggle, never a custom action.
 * Empty hold slots get power-off defaults; explicit hold mappings survive. */
static inline void a1_storage_panel_buttons_migrate(app_storage_t *s)
{
    if (s->automation.magic != AUTOMATION_MAGIC) return;
    automation_action_t *short_press = &s->automation.buttons[0][0];
    automation_action_t old = {0}; old.kind = AUTO_FAN; old.value = 2;
    old.target = short_press->target; /* Factory default 0; saved UI toggle 255. */
    if ((old.target != 0 && old.target != 255) || memcmp(short_press, &old, sizeof(old))) return;
    memset(short_press, 0, sizeof(*short_press)); short_press->kind = AUTO_FAN_PANEL;
    for (unsigned i = 30; i < 34; i++) {
        automation_action_t *hold = &s->automation.buttons[0][i];
        if (hold->kind != AUTO_NONE) continue;
        memset(hold, 0, sizeof(*hold)); hold->kind = AUTO_FAN; hold->target = 255;
    }
    ++s->automation.revision;
}

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

static inline void a1_storage_features_get(const app_storage_t *s, feature_config_t *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out, &s->features, sizeof(s->features));
    out->automatic = s->automatic;
    out->co2 = s->co2;
}
static inline void a1_storage_features_put(app_storage_t *s, const feature_config_t *in)
{
    memcpy(&s->features, in, sizeof(s->features));
    s->automatic = in->automatic;
    s->co2 = in->co2;
}
static inline void a1_storage_features_defaults(app_storage_t *s)
{
    feature_config_t next;
    feature_defaults(&next);
    a1_storage_features_put(s, &next);
}

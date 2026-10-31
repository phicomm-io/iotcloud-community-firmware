/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PH_POWER_RULES_MAX 8u
#define PH_POWER_LIMIT_MW UINT32_C(25000000)
#define PH_POWER_DURATION_MAX UINT32_C(604800)
enum { PH_POWER_ABOVE, PH_POWER_BELOW };
/* Fixed-width persistent values; no transport views, timers or pointers. */
typedef struct {
    uint16_t id;
    uint8_t enabled, comparison;
    uint32_t power_milliwatts, seconds;
    uint8_t target, on;
    uint16_t reserved;
} ph_power_rule;
typedef struct {
    uint32_t started;
    bool timing, fired;
} ph_power_trigger;

static inline bool ph_power_rule_valid(const ph_power_rule *r, unsigned outlets) {
    return r && r->id && r->enabled<=1 && r->comparison<=PH_POWER_BELOW &&
        r->power_milliwatts<=PH_POWER_LIMIT_MW && r->seconds<=PH_POWER_DURATION_MAX &&
        (r->target<outlets || r->target==255) && r->on<=1 && !r->reserved;
}
/* Monotonic time, including tick rollover. Invalid samples interrupt an
 * unfinished duration; only an observed false condition rearms a fired rule. */
static inline bool ph_power_trigger_due(ph_power_trigger *s, const ph_power_rule *r,
                                       uint32_t now, bool valid, uint32_t power_mw) {
    if(!r->enabled){s->timing=s->fired=false;return false;}
    if(!valid){s->timing=false;return false;}
    bool matches=r->comparison==PH_POWER_ABOVE?power_mw>r->power_milliwatts:power_mw<r->power_milliwatts;
    if(!matches){s->timing=s->fired=false;return false;}
    if(s->fired)return false;
    if(!s->timing){s->started=now;s->timing=true;}
    return (uint32_t)(now-s->started)>=r->seconds*1000u;
}
/* Commit only after the business operation succeeds. Busy is retried on the
 * next bounded poll while the condition still holds. */
static inline void ph_power_trigger_commit(ph_power_trigger *s) { s->fired=true; }
static inline void ph_power_trigger_override(ph_power_trigger *s) { s->timing=false;s->fired=true; }

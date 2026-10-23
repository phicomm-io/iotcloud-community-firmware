#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
/* Speeds are normalized commands, not measured RPM. Duty is actual high time. */
typedef struct {
    uint8_t coupling, active_low;
    uint16_t cap_ratio, boost_ratio, pulses_per_rev;
    float a, b, pwm_min, pwm_max;
} fresh_mapping_t;
typedef struct {
    bool requested_on, automatic, ready, fallback;
    unsigned requested_speed, target, applied, primary;
    unsigned reason; /* 0 none, 1 master off, 2 ratio/cap, 3 hardware, 4 reconfigure */
    float duty;
    uint32_t pulses, rpm;
} fresh_snapshot_t;
static inline bool fresh_mapping_valid(const fresh_mapping_t *m) {
    return m->coupling <= 2 && m->active_low <= 1 && m->cap_ratio <= 100 &&
        m->boost_ratio >= 1 && m->boost_ratio <= 500 &&
        m->pulses_per_rev >= 1 && m->pulses_per_rev <= 255 &&
        isfinite(m->a) && isfinite(m->b) && isfinite(m->pwm_min) && isfinite(m->pwm_max) &&
        m->pwm_min >= 0 && m->pwm_min <= m->pwm_max && m->pwm_max <= 100;
}
static inline float fresh_duty(const fresh_mapping_t *m, unsigned speed) {
    if (!speed) return m->active_low ? 100 : 0;
    double d = (double)m->a * speed + m->b;
    return d < m->pwm_min ? m->pwm_min : d > m->pwm_max ? m->pwm_max : (float)d;
}
static inline unsigned fresh_step(unsigned old, unsigned target) {
    return target > old ? (target-old > 10 ? old+10 : target) : (old-target > 10 ? old-10 : target);
}
/* 0: limit fresh, 1: raise primary, 2: independent. */
static inline unsigned fresh_limit(const fresh_mapping_t *m, unsigned primary) {
    return m->coupling==2 ? 100 : m->coupling==1 ? primary*100/m->boost_ratio : primary*m->cap_ratio/100;
}
/* Apply the physical invariant at EACH step, including a changed ratio/cap. */
static inline void fresh_pair(const fresh_mapping_t *m, unsigned desired_p, unsigned desired_f,
                              unsigned limit, unsigned old_p, unsigned old_f,
                              unsigned *p, unsigned *f) {
    if (desired_p > limit) desired_p = limit;
    if (m->coupling == 1) {
        unsigned needed = (desired_f * m->boost_ratio + 99) / 100;
        if (desired_p < needed) desired_p = needed > limit ? limit : needed;
    }
    *p = fresh_step(old_p, desired_p);
    if (*p > limit) *p = limit;
    unsigned cap = fresh_limit(m,*p);
    *f = fresh_step(old_f, desired_f);
    if (*f > cap) *f = cap;
}

#include "fresh_air.h"
#include <string.h>
void fresh_defaults(fresh_config_t *c) {
    memset(c, 0, sizeof(*c));
    c->magic=FRESH_MAGIC; c->schema=FRESH_SCHEMA; c->revision=1;
    c->speed=60; c->fallback_speed=30;
    c->mapping.coupling=2;
    c->mapping.cap_ratio=100; c->mapping.boost_ratio=100;
    c->mapping.pulses_per_rev=2;
    c->mapping.a=1; c->mapping.pwm_max=100;
    c->linkage.enabled=1; c->linkage.mode=1;
    c->linkage.metrics=c->linkage.on_metrics=c->linkage.off_metrics=4;
    c->linkage.fixed_speed=60; c->linkage.min_speed=20; c->linkage.max_speed=100;
    c->linkage.low_pm=10; c->linkage.high_pm=150; c->linkage.target_pm=35; c->linkage.on_pm=75;
    c->linkage.low_hcho=.01f; c->linkage.high_hcho=.2f; c->linkage.target_hcho=.08f; c->linkage.on_hcho=.1f;
    c->linkage.kp=1; c->linkage.ki=.01f;
    feature_co2_defaults(&c->co2);
}

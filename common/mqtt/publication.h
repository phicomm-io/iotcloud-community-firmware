/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>
/* One cursor per producer, owned by the MQTT thread. A successful HA send
 * must never acknowledge native state. Failed attempts retain the latest work
 * and have their own retry deadline; no payload queue or catch-up burst. */
typedef struct {
    uint32_t sent_at, attempted_at;
    bool sent, attempted;
} ph_mqtt_publication;
static inline bool ph_mqtt_publication_due(const ph_mqtt_publication *p,
    uint32_t now, uint32_t period, uint32_t retry, bool changed)
{
    return (!p->attempted || (uint32_t)(now-p->attempted_at)>=retry) &&
        (!p->sent || changed || (uint32_t)(now-p->sent_at)>=period);
}
static inline void ph_mqtt_publication_attempt(ph_mqtt_publication *p,uint32_t now)
{p->attempted=true;p->attempted_at=now;}
static inline void ph_mqtt_publication_complete(ph_mqtt_publication *p,uint32_t now)
{p->sent=true;p->sent_at=now;}

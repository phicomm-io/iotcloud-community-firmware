#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define M1_BAROMETER_STALE_MS 10000u

typedef struct {
    bool ready, received;
    uint8_t chip_id, address;
    uint32_t pressure_centi_pa, updated_ms, samples, errors, attempts;
    int32_t last_error;
} barometer_state_t;

static inline const char *m1_barometer_model(uint8_t chip_id)
{
    return chip_id == 0x50 ? "BMP388" : chip_id == 0x60 ? "BMP390" : NULL;
}

static inline bool m1_barometer_fresh(const barometer_state_t *s, uint32_t now)
{
    return s->ready && s->received &&
        (uint32_t)(now - s->updated_ms) < M1_BAROMETER_STALE_MS;
}

/* Serialized with SCD40 on the UART hardware owner task. */
void m1_barometer_tick(void);

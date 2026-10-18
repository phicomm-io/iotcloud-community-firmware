#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool ready, received;
    uint16_t co2_ppm;
    int32_t temperature_milli_c;
    uint32_t humidity_milli_pct, updated_ms, samples, errors, crc_errors;
    int32_t last_error;
    uint16_t last_command;
    uint8_t error_phase;
    bool bus_initialized;
    char serial[13];
} scd40_state_t;

/* Sensirion CRC-8: polynomial 0x31, initial value 0xff. */
static inline uint8_t scd40_crc(const uint8_t *p)
{
    uint8_t crc = 0xff;
    for (unsigned i = 0; i < 2; i++) {
        crc ^= p[i];
        for (unsigned b = 0; b < 8; b++)
            crc = (uint8_t)((crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1);
    }
    return crc;
}
static inline bool scd40_words(const uint8_t *p, uint16_t *words, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        if (scd40_crc(p + i * 3) != p[i * 3 + 2]) return false;
        words[i] = ((uint16_t)p[i * 3] << 8) | p[i * 3 + 1];
    }
    return true;
}
void m1_scd40_tick(void);

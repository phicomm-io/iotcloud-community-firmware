#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define M1_RX_FRAME_SIZE 20
#define M1_TX_FRAME_SIZE 12
#define M1_POLL_MS 5000u
#define M1_STALE_MS 15000u

typedef struct {
    uint16_t hcho_ug_m3, pm25_ug_m3;
    int32_t temperature_deci_c, humidity_deci_pct;
    uint32_t sequence, updated_ms;
    bool received;
} m1_sample_t;

typedef struct {
    uint8_t data[M1_RX_FRAME_SIZE];
    size_t used;
    uint32_t discarded;
} m1_decoder_t;

bool m1_decode_byte(m1_decoder_t *, uint8_t, uint8_t frame[M1_RX_FRAME_SIZE]);
bool m1_parse_sample(const uint8_t frame[M1_RX_FRAME_SIZE], uint32_t now, m1_sample_t *);
bool m1_sample_fresh(const m1_sample_t *, uint32_t now);
void m1_query_frame(uint8_t frame[M1_TX_FRAME_SIZE]);
void m1_brightness_frame(uint8_t frame[M1_TX_FRAME_SIZE], unsigned level, unsigned last_on);
void m1_clock_frame(uint8_t frame[M1_TX_FRAME_SIZE], unsigned year, unsigned month,
                    unsigned day, unsigned hour, unsigned minute, unsigned second);
uint16_t m1_crc16(uint16_t crc, const uint8_t *, size_t);


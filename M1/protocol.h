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
    uint8_t pm25_status, auxiliary_status, climate_status;
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

/* Byte 12 has not been identified as a particular sensor. Expose it raw. */
static inline unsigned m1_sample_fields(const m1_sample_t *s)
{ return (s->climate_status ? 3u : 0u) | (s->pm25_status ? 4u : 0u) | 8u; }
void m1_buzzer_frame(uint8_t frame[M1_TX_FRAME_SIZE], bool enabled);
void m1_version_frame(uint8_t frame[M1_TX_FRAME_SIZE]);
void m1_threshold_frame(uint8_t frame[M1_TX_FRAME_SIZE], bool pm25, uint16_t low, uint16_t high);
/* Sensor-alarm schema 1: bit 0 HCHO, bit 1 PM2.5, bit 2 legacy CO2. */
bool m1_sensor_alarm_frame(uint8_t frame[M1_TX_FRAME_SIZE], uint8_t sequence, bool write, uint8_t mask);
bool m1_parse_sensor_alarm(const uint8_t frame[M1_RX_FRAME_SIZE], uint8_t *mask, bool *active);
bool m1_parse_version(const uint8_t frame[M1_RX_FRAME_SIZE], char version[17]);
/* Built-in tones first appear in MCU v0.9.3; capability/PWM check is separate. */
bool m1_tone_version_supported(const char *version);


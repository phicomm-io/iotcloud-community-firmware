#include "protocol.h"
#include <string.h>

static uint16_t be16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }

bool m1_decode_byte(m1_decoder_t *d, uint8_t byte, uint8_t out[M1_RX_FRAME_SIZE])
{
    if (!d->used && byte != 0x23) { d->discarded++; return false; }
    d->data[d->used++] = byte;
    if (d->used < M1_RX_FRAME_SIZE) return false;
    if (d->data[0] == 0x23 && d->data[19] == 0x21) {
        memcpy(out, d->data, M1_RX_FRAME_SIZE);
        d->used = 0;
        return true;
    }
    /* Keep the next possible header, including one inside a corrupt frame. */
    size_t skip = 1;
    while (skip < d->used && d->data[skip] != 0x23) skip++;
    d->used -= skip;
    d->discarded += skip;
    memmove(d->data, d->data + skip, d->used);
    return false;
}

bool m1_parse_sample(const uint8_t f[M1_RX_FRAME_SIZE], uint32_t now, m1_sample_t *s)
{
    if (f[0] != 0x23 || f[1] != 1 || f[19] != 0x21) return false;
    s->hcho_ug_m3 = be16(f + 2);
    s->temperature_deci_c = (int32_t)f[5] * 10 + f[6] / 10;
    s->humidity_deci_pct = (int32_t)f[7] * 10 + f[8] / 10;
    s->pm25_ug_m3 = be16(f + 9);
    s->updated_ms = now;
    s->sequence++;
    s->received = true;
    return true;
}

bool m1_sample_fresh(const m1_sample_t *s, uint32_t now)
{ return s->received && (uint32_t)(now - s->updated_ms) < M1_STALE_MS; }

static void command(uint8_t *f, unsigned type)
{ memset(f, 0, M1_TX_FRAME_SIZE); f[0] = 0x23; f[1] = type; f[11] = 0x21; }
void m1_query_frame(uint8_t *f) { command(f, 1); }
void m1_brightness_frame(uint8_t *f, unsigned level, unsigned last_on)
{
    command(f, 2);
    f[2] = (level ? level : last_on) * 25;
    f[3] = level != 0;
}
void m1_clock_frame(uint8_t *f, unsigned y, unsigned mo, unsigned d,
                    unsigned h, unsigned mi, unsigned s)
{
    command(f, 3);
    f[2] = y >> 8; f[3] = y; f[4] = mo; f[5] = d;
    f[6] = h; f[7] = mi; f[8] = s;
}
uint16_t m1_crc16(uint16_t crc, const uint8_t *p, size_t n)
{
    while (n--) {
        crc ^= (uint16_t)*p++ << 8;
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}


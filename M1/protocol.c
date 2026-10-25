#include "protocol.h"
bool m1_tone_version_supported(const char *version)
{
    unsigned part[3]={0};
    for(unsigned i=0;i<3;i++) {
        if(*version<'0' || *version>'9')return false;
        do {
            part[i]=part[i]*10u+(unsigned)(*version++-'0');
            if(part[i]>999)return false;
        }while(*version>='0' && *version<='9');
        if(i<2 && *version++!='.')return false;
    }
    if(*version && (*version!='-' || !version[1]))return false;
    return part[0]>0 || part[1]>9 || (part[1]==9 && part[2]>=3);
}
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
    if (f[4] == 1) s->temperature_deci_c = -s->temperature_deci_c;
    s->humidity_deci_pct = (int32_t)f[7] * 10 + f[8] / 10;
    s->pm25_ug_m3 = be16(f + 9);
    s->pm25_status = f[11]; s->auxiliary_status = f[12]; s->climate_status = f[13];
    s->updated_ms = now;
    s->sequence++;
    s->received = true;
    return true;
}

bool m1_sample_fresh(const m1_sample_t *s, uint32_t now)
{ return s->received && (uint32_t)(now - s->updated_ms) < M1_STALE_MS; }

static void command(uint8_t *f, unsigned type)
{ memset(f, 0, M1_TX_FRAME_SIZE); f[0] = 0x23; f[1] = type; f[11] = 0x21; }
void m1_query_frame(uint8_t f[M1_TX_FRAME_SIZE]) { command(f, 1); }
void m1_brightness_frame(uint8_t f[M1_TX_FRAME_SIZE], unsigned level, unsigned last_on)
{
    command(f, 2);
    f[2] = (level ? level : last_on) * 25;
    f[3] = level != 0;
}
void m1_clock_frame(uint8_t f[M1_TX_FRAME_SIZE], unsigned y, unsigned mo, unsigned d,
                    unsigned h, unsigned mi, unsigned s)
{
    command(f, 3);
    f[2] = y >> 8; f[3] = y; f[4] = mo; f[5] = d;
    f[6] = h; f[7] = mi; f[8] = s;
}
void m1_buzzer_frame(uint8_t f[M1_TX_FRAME_SIZE], bool enabled)
{ command(f, 0x0e); f[2] = enabled; }
void m1_version_frame(uint8_t f[M1_TX_FRAME_SIZE]) { command(f, 0x11); }
void m1_threshold_frame(uint8_t f[M1_TX_FRAME_SIZE], bool pm25, uint16_t low, uint16_t high)
{
    command(f, pm25 ? 0x06 : 0x05);
    f[2] = low >> 8; f[3] = low; f[4] = high >> 8; f[5] = high;
}
bool m1_sensor_alarm_frame(uint8_t f[M1_TX_FRAME_SIZE], uint8_t sequence, bool write, uint8_t mask)
{
    if ((mask & ~7u) || (!write && mask)) return false;
    command(f, 0x70); f[2] = 20; f[3] = sequence; f[4] = write; f[5] = mask;
    return true;
}
bool m1_parse_sensor_alarm(const uint8_t f[M1_RX_FRAME_SIZE], uint8_t *mask, bool *active)
{
    if (f[0] != 0x23 || f[1] != 0x70 || f[2] != 20 || f[19] != 0x21 ||
        f[4] || f[5] != 1 || (f[6] & ~7u) || f[7] > 1) return false;
    *mask = f[6]; *active = f[7] != 0;
    return true;
}
bool m1_parse_version(const uint8_t f[M1_RX_FRAME_SIZE], char version[17])
{
    unsigned n = f[2];
    if (f[0] != 0x23 || f[1] != 0x11 || f[19] != 0x21 || !n || n > 16) return false;
    while (n && !f[3+n-1]) n--;
    if (!n) return false;
    for (unsigned i = 0; i < n; i++) if (f[3+i] < 32 || f[3+i] > 126) return false;
    memcpy(version, f + 3, n); version[n] = 0; return true;
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


/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "media_wire.h"
#include <string.h>
uint32_t media_u32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
void media_put32(uint8_t *p, uint32_t v) {
  for (unsigned i = 0; i < 4; i++)
    p[i] = v >> (8 * i);
}
uint32_t media_crc32(const void *v, size_t n) {
  const uint8_t *p = v;
  uint32_t c = ~0u;
  while (n--) {
    c ^= *p++;
    for (unsigned i = 0; i < 8; i++)
      c = (c >> 1) ^ (0xedb88320u & -(c & 1));
  }
  return ~c;
}
bool media_rate_valid(uint32_t rate) {
  return rate == 115200 || rate == 230400 || rate == 460800 || rate == 921600;
}
bool media_feed(media_parser *s, unsigned c, uint32_t now) {
  if (s->active && (uint32_t)(now - s->at) > 1000) {
    s->active = false;
    s->framing_errors++;
  }
  if (c == 0) {
    if (!s->active) {
      s->active = true;
      s->used = 0;
      s->escape = s->discard = false;
      s->at = now;
      return true;
    }
    /* Repeated delimiters also recover after a lost closing delimiter. */
    if (!s->used && !s->discard && !s->escape) {
      s->at = now;
      return true;
    }
    s->active = false;
    if (s->discard || s->escape || s->used < 18) {
      s->framing_errors++;
      return true;
    }
    uint8_t *p = s->bytes;
    size_t n = (unsigned)p[12] | (unsigned)p[13] << 8;
    if (p[0] != 'G' || p[1] != 'M' || p[2] != 1 || p[3] > MEDIA_SOCKET_EVENT ||
        n > MEDIA_PACKET_MAX || s->used != n + 18) {
      s->framing_errors++;
      return true;
    }
    if (media_crc32(p, n + 14) != media_u32(p + n + 14)) {
      s->crc_errors++;
      return true;
    }
    if (s->packet)
      s->packet(s->context, p[3], media_u32(p + 4), media_u32(p + 8), p + 14,
                n);
    return true;
  }
  if (!s->active)
    return false;
  s->at = now;
  if (s->discard)
    return true;
  if (s->escape) {
    s->escape = false;
    if (c == 0xdc)
      c = 0;
    else if (c == 0xdd)
      c = 0xdb;
    else {
      s->discard = true;
      return true;
    }
  } else if (c == 0xdb) {
    s->escape = true;
    return true;
  }
  if (s->used == sizeof(s->bytes))
    s->discard = true;
  else
    s->bytes[s->used++] = c;
  return true;
}
size_t media_encode(uint8_t *out, size_t cap, unsigned channel,
                    uint32_t session, uint32_t seq, const void *data,
                    size_t n) {
  if (n > MEDIA_PACKET_MAX || channel > MEDIA_SOCKET_EVENT || (!data && n) ||
      !session || !cap)
    return 0;
  uint8_t p[MEDIA_PACKET_MAX + 18] = {'G', 'M', 1, (uint8_t)channel};
  media_put32(p + 4, session);
  media_put32(p + 8, seq);
  p[12] = n;
  p[13] = n >> 8;
  if (n)
    memcpy(p + 14, data, n);
  media_put32(p + 14 + n, media_crc32(p, n + 14));
  size_t used = 1;
  out[0] = 0;
  for (size_t i = 0; i < n + 18; i++) {
    bool escape = p[i] == 0 || p[i] == 0xdb;
    if (used + (escape ? 2 : 1) >= cap)
      return 0;
    if (escape) {
      out[used++] = 0xdb;
      out[used++] = p[i] ? 0xdd : 0xdc;
    } else
      out[used++] = p[i];
  }
  out[used++] = 0;
  return used;
}

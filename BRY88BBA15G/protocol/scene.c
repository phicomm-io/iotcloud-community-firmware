#include "scene.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
uint32_t scene_u32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
void scene_put32(uint8_t *p, uint32_t n) {
  for (unsigned i = 0; i < 4; i++)
    p[i] = (uint8_t)(n >> (i * 8));
}
uint32_t scene_crc32(const void *v, size_t n) {
  const uint8_t *p = v;
  uint32_t c = ~0u;
  while (n--) {
    c ^= *p++;
    for (unsigned i = 0; i < 8; i++)
      c = (c >> 1) ^ (0xedb88320u & -(c & 1));
  }
  return ~c;
}
static bool utf8(const char *s, size_t cap, bool lines) {
  size_t i = 0;
  while (i < cap && s[i]) {
    uint8_t a = (uint8_t)s[i++];
    if ((a < 0x20 && !(lines && a == '\n')) || a == 0x7f)
      return false;
    if (a < 0x80)
      continue;
    unsigned n;
    uint32_t cp, min;
    if (a >= 0xc2 && a <= 0xdf) {
      n = 1;
      cp = a & 31;
      min = 0x80;
    } else if (a >= 0xe0 && a <= 0xef) {
      n = 2;
      cp = a & 15;
      min = 0x800;
    } else if (a >= 0xf0 && a <= 0xf4) {
      n = 3;
      cp = a & 7;
      min = 0x10000;
    } else
      return false;
    if (n >= cap - i)
      return false;
    while (n--) {
      uint8_t b = (uint8_t)s[i++];
      if ((b & 0xc0) != 0x80)
        return false;
      cp = cp << 6 | (b & 63);
    }
    if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
      return false;
  }
  return i < cap;
}
bool scene_utf8(const char *s, size_t cap) { return utf8(s, cap, false); }
bool scene_validate(const screen_scene *s) {
  if (!s || s->schema != 3 || !s->pages || s->pages > SCENE_PAGES || s->count > SCENE_ITEMS)
    return false;
  uint32_t cells[SCENE_PAGES] = {0};
  unsigned counts[SCENE_PAGES] = {0}, enabled = 0;
  for (unsigned p = 0; p < s->pages; p++) {
    if (!scene_utf8(s->page[p].title, 32))
      return false;
    if (s->schema == 3) {
      if (!s->page[p].id || s->page[p].id > 0xffffff) return false;
      for (unsigned j = 0; j < p; j++) if (s->page[j].id == s->page[p].id) return false;
    }
    enabled += s->page[p].enabled;
  }
  if (!enabled)
    return false;
  for (unsigned i = 0; i < s->count; i++) {
    const scene_item *c = &s->item[i];
    if (!c->id || c->page >= s->pages || c->type > SC_REMINDERS ||
        c->binding > SC_BIND_EXTERNAL || c->source > 2 || !c->w || !c->h ||
        c->x + c->w > 4 || c->y + c->h > 6 || ++counts[c->page] > 8 ||
        !scene_utf8(c->title, 49) || !scene_utf8(c->unit, 17))
      return false;
    if (s->schema == 3) {
      bool chart = c->type == SC_LINE || c->type == SC_BAR || c->type == SC_PIE;
      if (c->w < (chart ? 4 : 2) ||
          c->h < (c->type == SC_VALUE && c->w == 4 ? 1 : 2) ||
          c->chart > SC_CH_RING || c->series_count > SCENE_SERIES ||
          (c->hours && c->hours != 1 && c->hours != 6 && c->hours != 24) ||
          c->binding2 > SC_BIND_EXTERNAL || c->binding3 > SC_BIND_EXTERNAL ||
          c->additive > 1 || !utf8(c->text, sizeof(c->text), true) ||
          (c->points&&c->points!=32&&c->points!=64&&c->points!=128&&c->points!=256) || c->hide_title > 1 || c->value_mode > SC_VALUE_GRADE ||
          (c->value_mode && c->type != SC_VALUE) ||
          (c->value_mode == SC_VALUE_GRADE && c->binding != SC_BIND_TVOC &&
           c->binding != SC_BIND_EXTERNAL)) return false;
      if (chart && (c->chart == SC_CH_STACK_BAR || c->chart == SC_CH_STACK_AREA ||
                     c->chart == SC_CH_RING) &&
          (c->binding != SC_BIND_EXTERNAL || !c->additive)) return false;
      if(chart&&c->binding!=SC_BIND_EXTERNAL) {
        unsigned bindings[]={c->binding,c->binding2,c->binding3};
        unsigned count=c->chart==SC_CH_LINE||c->chart==SC_CH_BAR?1:c->series_count?c->series_count:2;
        for(unsigned k=0;k<count;k++)if(bindings[k]==SC_BIND_HCHO||bindings[k]==SC_BIND_EXTERNAL)return false;
        if(c->chart==SC_CH_DUAL&&count>2&&bindings[0]!=bindings[1]&&bindings[1]!=bindings[2]&&bindings[0]!=bindings[2])return false;
      }
    }
    for (unsigned j = 0; j < i; j++)
      if (s->item[j].id == c->id)
        return false;
    uint32_t mask = 0;
    for (unsigned y = c->y; y < c->y + c->h; y++)
      for (unsigned x = c->x; x < c->x + c->w; x++)
        mask |= 1u << (y * 4 + x);
    if (cells[c->page] & mask)
      return false;
    cells[c->page] |= mask;
  }
  return true;
}
bool scene_requires_multi_units(const scene_item *c) {
 if((c->type!=SC_LINE&&c->type!=SC_BAR&&c->type!=SC_PIE)||c->binding==SC_BIND_EXTERNAL)return false;
 if(c->binding==SC_BIND_TVOC)return true;
 if(c->chart!=SC_CH_MULTI&&c->chart!=SC_CH_DUAL)return false;
 unsigned count=c->series_count?c->series_count:2;
 return (count>1&&(c->binding2==SC_BIND_TVOC||c->binding2!=c->binding))||
        (count>2&&(c->binding3==SC_BIND_TVOC||c->binding3!=c->binding));
}
void scene_defaults(screen_scene *s) {
  memset(s, 0, sizeof(*s));
  s->revision = 1;
  s->schema = 3;
  s->pages = 6;
  const char *titles[] = {"时钟",     "空气数据", "变化趋势", "户外天气",
                          "提醒事项", "外部数据"};
  for (unsigned i = 0; i < s->pages; i++) {
    s->page[i].enabled = true;
    strcpy(s->page[i].title, titles[i]);
    s->page[i].id = i + 1;
  }
  scene_item items[] = {
      {1, 0, SC_CLOCK, 0, 0, 0, 4, 2, 0, "时钟", "", .text = ""},
      {2, 0, SC_VALUE, SC_BIND_CO2, 0, 2, 2, 2, 0, "CO2", "ppm", .text = ""},
      {3, 0, SC_VALUE, SC_BIND_PM25, 2, 2, 2, 2, 0, "PM2.5", "ug/m3", .text = ""},
      {4, 0, SC_VALUE, SC_BIND_TEMP, 0, 4, 2, 2, 0, "温度", "C", .text = ""},
      {5, 0, SC_VALUE, SC_BIND_HUM, 2, 4, 2, 2, 0, "湿度", "%", .text = ""},
      {6, 1, SC_VALUE, SC_BIND_CO2, 0, 0, 2, 2, 0, "CO2", "ppm", .text = ""},
      {7, 1, SC_VALUE, SC_BIND_PM25, 2, 0, 2, 2, 0, "PM2.5", "ug/m3", .text = ""},
      {8, 1, SC_VALUE, SC_BIND_TEMP, 0, 2, 2, 2, 0, "温度", "C", .text = ""},
      {9, 1, SC_VALUE, SC_BIND_HUM, 2, 2, 2, 2, 0, "湿度", "%", .text = ""},
      {10, 1, SC_VALUE, SC_BIND_TVOC, 0, 4, 2, 2, 0, "TVOC", "", .text = ""},
      {11, 1, SC_VALUE, SC_BIND_HCHO, 2, 4, 2, 2, 0, "甲醛", "raw", .text = ""},
      {12, 2, SC_LINE, SC_BIND_CO2, 0, 0, 4, 6, 0, "CO2", "ppm", .text = ""},
      {14, 3, SC_WEATHER, 0, 0, 0, 4, 6, 0, "户外天气", "", .text = ""},
      {15, 4, SC_REMINDERS, 0, 0, 0, 4, 6, 0, "提醒事项", "", .text = ""},
      {16, 5, SC_TEXT, SC_BIND_EXTERNAL, 0, 0, 4, 6, 0, "外部数据", "", .text = ""}};
  s->count = sizeof(items) / sizeof(items[0]);
  memcpy(s->item, items, s->count * sizeof(items[0]));
  for (unsigned i = 0; i < s->count; i++) {
    s->item[i].hours = 24; s->item[i].series_count = 1;
  }
}
size_t scene_wire_size(const screen_scene *s) {
  return s && s->schema == 3 ? SCENE_BYTES : 0;
}
bool scene_encode(const screen_scene *s, uint8_t out[SCENE_BYTES]) {
  if (!scene_validate(s)) return false;
  unsigned stride = 288;
  size_t size = SCENE_BYTES;
  memset(out, 0, size); memcpy(out, "SCN3", 4);
  scene_put32(out + 4, s->revision); out[8] = s->pages; out[9] = s->count;
  for (unsigned i = 0; i < s->pages; i++) {
    uint8_t *p = out + 16 + 36 * i;
    p[0] = s->page[i].enabled; memcpy(p + 4, s->page[i].title, 32);
    { p[1] = s->page[i].id; p[2] = s->page[i].id >> 8; p[3] = s->page[i].id >> 16; }
  }
  for (unsigned i = 0; i < s->count; i++) {
    const scene_item *c = &s->item[i]; uint8_t *p = out + 304 + stride * i;
    p[0] = c->id; p[1] = c->id >> 8; p[2] = c->page;
    p[3] = c->type; p[4] = c->binding; p[5] = c->x; p[6] = c->y;
    p[7] = c->w; p[8] = c->h; p[9] = c->source;
    memcpy(p + 16, c->title, 49); memcpy(p + 65, c->unit, 17);
    {
      p[10] = c->chart; p[11] = c->hours; p[12] = c->series_count;
      p[13] = c->binding2; p[14] = c->binding3; p[15] = c->additive;
      memcpy(p + 88, c->text, 193);
      p[82] = c->value_mode; p[83] = c->hide_title; p[84]=c->points; p[85]=c->points>>8;
    }
  }
  return true;
}
bool scene_decode(screen_scene *s, const uint8_t *in, size_t n) {
  if (n != SCENE_BYTES || memcmp(in, "SCN3", 4)) return false;
  /* ESP's UART owner has an 8-KiB stack. Decode atomically on the bounded
   * heap rather than reserving a complete scene in every command frame. */
  screen_scene *next = calloc(1, sizeof(*next));
  if (!next) return false;
  next->schema = 3; next->revision = scene_u32(in + 4);
  next->pages = in[8]; next->count = in[9];
  bool ok = next->pages <= SCENE_PAGES && next->count <= SCENE_ITEMS;
  for (unsigned i = 0; ok && i < next->pages; i++) {
    const uint8_t *p = in + 16 + 36 * i;
    ok = p[0] <= 1; next->page[i].enabled = p[0];
    next->page[i].id = (uint32_t)p[1] | (uint32_t)p[2] << 8 | (uint32_t)p[3] << 16;
    memcpy(next->page[i].title, p + 4, 32);
  }
  for (unsigned i = 0; ok && i < next->count; i++) {
    const uint8_t *p = in + 304 + 288 * i;
    scene_item *c = &next->item[i];
    c->id = p[0] | p[1] << 8; c->page = p[2]; c->type = p[3];
    c->binding = p[4]; c->x = p[5]; c->y = p[6]; c->w = p[7]; c->h = p[8];
    c->source = p[9]; memcpy(c->title, p + 16, 49); memcpy(c->unit, p + 65, 17);
    {
      c->chart = p[10]; c->hours = p[11]; c->series_count = p[12];
      c->binding2 = p[13]; c->binding3 = p[14]; c->additive = p[15];
      memcpy(c->text, p + 88, 193);
      c->value_mode = p[82]; c->hide_title = p[83]; c->points=p[84]|p[85]<<8;
    }
  }
  ok = ok && scene_validate(next);
  if (ok) *s = *next;
  free(next); return ok;
}
bool scene_receive(scene_transaction *t, screen_scene *s, const uint8_t *p,
                   size_t n) {
  if (!n)
    return false;
  if (p[0] == SC_MSG_BEGIN) {
    t->active = false;
    if (n != 13 || scene_u32(p + 5) != SCENE_BYTES)
      return false;
    t->total = scene_u32(p + 5);
    t->revision = scene_u32(p + 1);
    t->crc = scene_u32(p + 9);
    t->received = 0;
    t->active = true;
    return true;
  }
  if (!t->active)
    return false;
  if (p[0] == SC_MSG_CHUNK) {
    if (n <= 5 || scene_u32(p + 1) != t->received ||
        n - 5 > t->total - t->received) {
      t->active = false;
      return false;
    }
    memcpy(t->bytes + t->received, p + 5, n - 5);
    t->received += n - 5;
    return true;
  }
  if (p[0] == SC_MSG_COMMIT) {
    t->active = false;
    if (n != 5 || scene_u32(p + 1) != t->revision ||
        t->received != t->total ||
        scene_crc32(t->bytes, t->total) != t->crc ||
        scene_u32(t->bytes + 4) != t->revision)
      return false;
    return scene_decode(s, t->bytes, t->total);
  }
  return false;
}
bool scene_data_fresh(const scene_data *d, uint32_t now) {
  return d && d->valid && !d->expired && (uint32_t)(now - d->at) < d->ttl;
}
bool scene_data_receive(scene_data *d, const uint8_t *p, size_t n,
                        uint32_t now) {
  if (n < 9 || p[0] != SC_MSG_DATA || p[3] > SCENE_POINTS || p[8] > 192 ||
      n != 9u + p[8] + 4u * p[3])
    return false;
  uint32_t ttl = scene_u32(p + 4);
  if (!ttl || ttl > 86400)
    return false;
  scene_data v = {0};
  v.id = p[1] | p[2] << 8;
  v.count = p[3];
  v.ttl = ttl * 1000;
  v.at = now;
  v.valid = true;
  memcpy(v.text, p + 9, p[8]);
  if (!v.id || !scene_utf8(v.text, sizeof(v.text)))
    return false;
  for (unsigned i = 0; i < v.count; i++) {
    uint32_t bits = scene_u32(p + 9 + p[8] + 4 * i);
    memcpy(&v.values[i], &bits, 4);
    if (!isfinite(v.values[i]) || fabsf(v.values[i]) > 1e9f)
      return false;
  }
  *d = v;
  return true;
}
bool scene_series_receive(scene_data *d, const uint8_t *p, size_t n, uint32_t now) {
  if (n < 12 || p[0] != SC_MSG_SERIES || p[1] != 1 || !p[4] ||
      p[4] > SCENE_SERIES || !p[5] || p[5] > SCENE_SERIES_POINTS ||
      p[10] || p[11]) return false;
  unsigned mask_bytes = (p[5] + 7) / 8;
  unsigned stride = 48 + mask_bytes + 4 * p[5];
  uint32_t ttl = scene_u32(p + 6);
  if (n != 12u + p[4] * stride || !ttl || ttl > 86400) return false;
  scene_data next = {0};
  next.id = p[2] | p[3] << 8; next.series_count = p[4]; next.count = p[5];
  next.valid = true; next.at = now; next.ttl = ttl * 1000;
  if (!next.id) return false;
  for (unsigned k = 0; k < next.series_count; k++) {
    const uint8_t *q = p + 12 + stride * k;
    memcpy(next.names[k], q, 32); memcpy(next.units[k], q + 32, 16);
    if (!scene_utf8(next.names[k], 32) || !scene_utf8(next.units[k], 16)) return false;
    for (unsigned b = 0; b < mask_bytes; b++) next.masks[k] |= (uint32_t)q[48 + b] << (8 * b);
    if (next.count < 32 && (next.masks[k] >> next.count)) return false;
    for (unsigned i = 0; i < next.count; i++) {
      uint32_t bits = scene_u32(q + 48 + mask_bytes + 4 * i);
      memcpy(&next.series[k][i], &bits, 4);
      if (!isfinite(next.series[k][i]) || fabsf(next.series[k][i]) > 1e9f) return false;
    }
  }
  memcpy(next.values, next.series[0], next.count * sizeof(float));
  *d = next; return true;
}

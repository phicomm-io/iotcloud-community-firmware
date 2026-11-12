#include "store.h"
#include "protocol/scene.h"
#include <string.h>
static const uint8_t marker[16] = {'I',  'O',  'T',  'C',  'S',  'C', 'R', '2',
                                   0x88, 0xb1, 0x05, 0x26, 0x70, 0,   0,   1};
static const uint32_t slots[] = {0x80000, 0x90000};
static bool read_at(bry_store *s, uint32_t a, void *b, size_t n) {
  return s->read && a <= BRY_STORE_SIZE && n <= BRY_STORE_SIZE - a &&
         s->read(s->context, a, b, n);
}
bool bry_store_probe(bry_store *s) {
  uint8_t b[256];
  s->owned = s->blank = false;
  s->generation = 0;
  s->slot = 0;
  if (!read_at(s, 0, b, sizeof(b)))
    return false;
  if (!memcmp(b, marker, 16) && scene_u32(b + 16) == scene_crc32(marker, 16)) {
    s->owned = true;
    return true;
  }
  for (uint32_t a = 0; a < BRY_STORE_SIZE; a += sizeof(b)) {
    if (!read_at(s, a, b, sizeof(b)))
      return false;
    for (unsigned i = 0; i < sizeof(b); i++)
      if (b[i] != 0xff)
        return false;
  }
  s->blank = true;
  return true;
}
bool bry_store_claim(bry_store *s) {
  if (s->owned)
    return true;
  if (!s->blank || !s->write || !bry_store_probe(s) || !s->blank)
    return false;
  uint8_t b[20], check[20];
  memcpy(b, marker, 16);
  scene_put32(b + 16, scene_crc32(marker, 16));
  if (!s->write(s->context, 0, b, sizeof(b)) ||
      !read_at(s, 0, check, sizeof(check)) || memcmp(b, check, sizeof(b)))
    return false;
  s->owned = true;
  s->blank = false;
  return true;
}
static bool valid_slot(bry_store *s, unsigned index, uint32_t *generation,
                       uint32_t *length) {
  uint8_t h[24];
  uint32_t a = slots[index];
  if (!read_at(s, a, h, sizeof(h)) || memcmp(h, "SCFG2\0\0\0", 8) ||
      scene_u32(h + 20) != 0x52454332u)
    return false;
  *generation = scene_u32(h + 8);
  *length = scene_u32(h + 12);
  if (!*generation || !*length || *length > BRY_CONFIG_LIMIT)
    return false;
  /* The config is at most 48 KB. Stream the CRC without a second full copy. */
  uint32_t crc = ~0u;
  uint8_t block[256];
  for (uint32_t at = 0; at < *length;) {
    size_t n = *length - at;
    if (n > sizeof(block))
      n = sizeof(block);
    if (!read_at(s, a + 24 + at, block, n))
      return false;
    for (size_t i = 0; i < n; i++) {
      crc ^= block[i];
      for (unsigned bit = 0; bit < 8; bit++)
        crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    at += n;
  }
  return ~crc == scene_u32(h + 16);
}
bool bry_store_load(bry_store *s, void *out, size_t cap, size_t *size) {
  if (!s->owned || !size || (!out && cap))
    return false;
  uint32_t gen[2] = {0}, len[2] = {0};
  bool good[2];
  for (unsigned i = 0; i < 2; i++)
    good[i] = valid_slot(s, i, &gen[i], &len[i]);
  if (!good[0] && !good[1])
    return false;
  unsigned index = good[1] && (!good[0] || gen[1] > gen[0]);
  s->generation = gen[index];
  s->slot = index;
  *size = len[index];
  if (!out)
    return true;
  if (len[index] > cap)
    return false;
  if (!read_at(s, slots[index] + 24, out, len[index]))
    return false;
  return true;
}
typedef struct {
  bry_store *store;
  uint32_t address, crc;
  size_t size, written;
  bool failed;
} store_stream;
static bool stream_write(void *context, const void *data, size_t n) {
  store_stream *stream = context;
  if (stream->failed) return false;
  if ((!data && n) || n > stream->size - stream->written ||
      (n && !stream->store->write(stream->store->context,
         stream->address + stream->written, data, n))) {
    stream->failed = true;
    return false;
  }
  const uint8_t *bytes = data;
  for (size_t i = 0; i < n; i++) {
    stream->crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; bit++)
      stream->crc = (stream->crc >> 1) ^
                    ((stream->crc & 1) ? 0xedb88320u : 0);
  }
  stream->written += n;
  return true;
}
bool bry_store_save_stream(bry_store *s, size_t n,
                          bry_store_producer produce, void *context) {
  if (!produce || !n || n > BRY_CONFIG_LIMIT || !s->erase || !s->write ||
      s->generation == UINT32_MAX || !bry_store_claim(s))
    return false;
  unsigned index = s->generation ? 1 - s->slot : 0;
  uint32_t a = slots[index];
  if (!s->erase(s->context, a, 0x10000))
    return false;
  store_stream stream = {.store = s, .address = a + 24, .crc = ~0u,
                         .size = n, .written = 0, .failed = false};
  if (!produce(context, stream_write, &stream) || stream.failed || stream.written != n)
    return false;
  uint8_t h[24];
  memcpy(h, "SCFG2\0\0\0", 8);
  scene_put32(h + 8, s->generation + 1);
  scene_put32(h + 12, n);
  scene_put32(h + 16, ~stream.crc);
  scene_put32(h + 20, 0x52454332);
  if (!s->write(s->context, a, h, sizeof(h)))
    return false;
  uint32_t gen, len;
  if (!valid_slot(s, index, &gen, &len))
    return false;
  s->generation = gen;
  s->slot = index;
  return true;
}
typedef struct { const void *data; size_t size; } memory_source;
static bool produce_memory(void *context, bry_store_sink sink, void *target) {
  const memory_source *source = context;
  return sink(target, source->data, source->size);
}
bool bry_store_save(bry_store *s, const void *data, size_t n) {
  if (!data) return false;
  memory_source source = {data, n};
  return bry_store_save_stream(s, n, produce_memory, &source);
}

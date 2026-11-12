#ifndef SCREEN_RESOURCES_H
#define SCREEN_RESOURCES_H
#ifdef __cplusplus
extern "C" {
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Legacy bank remains read-only. Managed banks are below 16 MiB and cannot
 * overlap factory assets, OTA staging/copy/flags or the wallpaper banks. */
#define SCREEN_RESOURCE_BASE 0x900000u
#define SCREEN_FONT_BASE 0xb00000u
#define SCREEN_FONT_BANK 0x200000u
#define SCREEN_RESOURCE_LIMIT (SCREEN_FONT_BANK-8192u)
/* Keep large-bank loading asynchronous and bounded, including when hardware
 * SFC reads fall back to conservative GPIO timing. */
#define SCREEN_FONT_DEADLINE 300000u
#define SCREEN_MSG_FONT 0x33u
#define SCREEN_MSG_FONT_STATUS 0x34u
#define FONT_PREPARING 7u
enum { RESOURCE_UNCHECKED, RESOURCE_ABSENT, RESOURCE_READY, RESOURCE_IO,
       RESOURCE_INVALID, RESOURCE_TIMEOUT, RESOURCE_UPLOADING, RESOURCE_PREPARING,
       RESOURCE_LOADING };
typedef struct {
  void *context;
  bool (*read)(void *, uint32_t, void *, size_t);
  uint32_t (*now)(void *);
  void (*service)(void *);
  uint8_t *cache;
  size_t capacity;
  uint32_t bytes, crc, base, alternate, expected, received, started, generation;
  uint32_t expected_bytes, alternate_generation, alternate_expected, alternate_bytes;
  uint32_t whole_crc, payload_crc, elapsed_ms;
  uint16_t count;
  uint8_t state, version, bpp, pixels;
} screen_resources;
typedef struct {
  uint16_t advance, width, height;
  int16_t x, y;
  const uint8_t *pixels;
  uint8_t bpp;
} resource_glyph;
bool resources_header(const uint8_t *,size_t);
bool resources_validate(screen_resources *,size_t);
void resources_start(screen_resources *);
void resources_poll(screen_resources *);
bool resources_load(screen_resources *);
bool resources_glyph(const screen_resources *, uint32_t, resource_glyph *);
bool resources_bank(const uint8_t *,uint32_t *,uint32_t *,uint32_t *);
extern screen_resources screen_font_resources;
#ifdef __cplusplus
}
#endif

#endif /* SCREEN_RESOURCES_H */

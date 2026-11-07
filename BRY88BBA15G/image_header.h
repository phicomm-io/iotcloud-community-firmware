/* ESP-local staged screen image metadata. Never sent to the screen MCU. */
#pragma once
#include "protocol/scene.h"
#include <string.h>
#define BRY_IMAGE_HEADER_BYTES 80
static inline void bry_image_header_encode(uint8_t h[BRY_IMAGE_HEADER_BYTES],
                                          uint32_t size, const uint8_t md5[16],
                                          const uint8_t sha[32], uint32_t features) {
  memset(h, 0, BRY_IMAGE_HEADER_BYTES);
  memcpy(h, "SMCUIMG3", 8);
  scene_put32(h + 8, size);
  memcpy(h + 12, md5, 16);
  memcpy(h + 28, sha, 32);
  scene_put32(h + 60, features);
  scene_put32(h + 76, scene_crc32(h, 76));
}
static inline bool bry_image_header_decode(const uint8_t h[BRY_IMAGE_HEADER_BYTES],
                                          uint32_t size, uint8_t md5[16],
                                          uint8_t sha[32], uint32_t *features) {
  uint32_t flags = 0;
  if (scene_u32(h + 8) != size)
    return false;
  if (memcmp(h, "SMCUIMG3", 8)) return false;
  {
    if (scene_u32(h + 76) != scene_crc32(h, 76))
      return false;
    flags = scene_u32(h + 60);
    if (flags & ~3u)
      return false;
    for (unsigned i = 64; i < 76; i++)
      if (h[i])
        return false;
  }
  memcpy(md5, h + 12, 16);
  memcpy(sha, h + 28, 32);
  *features = flags;
  return true;
}

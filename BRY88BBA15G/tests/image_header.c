#include "image_header.h"
#include <assert.h>
int main(void) {
  uint8_t h[BRY_IMAGE_HEADER_BYTES], md5[16], sha[32], m[16], s[32];
  uint32_t features = 0;
  for (unsigned i = 0; i < sizeof(md5); i++) md5[i] = (uint8_t)(i + 1);
  for (unsigned i = 0; i < sizeof(sha); i++) sha[i] = (uint8_t)(i * 7 + 3);
  bry_image_header_encode(h, 421888, md5, sha, 3);
  assert(bry_image_header_decode(h, 421888, m, s, &features));
  assert(features == 3 && !memcmp(m, md5, 16) && !memcmp(s, sha, 32));
  assert(!bry_image_header_decode(h, 81920, m, s, &features));
  for (unsigned i = 0; i < sizeof(h); i++) {
    h[i] ^= 0x80;
    assert(!bry_image_header_decode(h, 421888, m, s, &features));
    h[i] ^= 0x80;
  }
  /* Re-upload old staged images using the current metadata format. */
  memcpy(h, "SMCUIMG2", 8);
  scene_put32(h + 60, scene_crc32(h, 60));
  assert(!bry_image_header_decode(h, 421888, m, s, &features));
  return 0;
}

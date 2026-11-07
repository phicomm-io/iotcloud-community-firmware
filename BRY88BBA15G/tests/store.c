#include "store.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned char flash[BRY_STORE_SIZE], baseline[BRY_STORE_SIZE];
static long budget = -1;
static unsigned mutations;
static bool read_flash(void *c, uint32_t a, void *p, size_t n) {
  (void)c;
  assert(a + n <= sizeof(flash));
  memcpy(p, flash + a, n);
  return true;
}
static bool write_flash(void *c, uint32_t a, const void *p, size_t n) {
  (void)c;
  assert(a + n <= sizeof(flash));
  mutations++;
  size_t done = n;
  if (budget >= 0 && done > (size_t)budget)
    done = budget;
  for (size_t i = 0; i < done; i++)
    flash[a + i] &= ((const unsigned char *)p)[i];
  if (budget >= 0)
    budget -= done;
  return done == n;
}
static bool erase_flash(void *c, uint32_t a, size_t n) {
  (void)c;
  assert(a % 4096 == 0 && n % 4096 == 0 && a + n <= sizeof(flash));
  mutations++;
  if (!budget)
    return false;
  memset(flash + a, 255, n);
  return true;
}
static bry_store owner(void) {
  bry_store s = {
      .read = read_flash, .write = write_flash, .erase = erase_flash};
  assert(bry_store_probe(&s));
  return s;
}
static uint8_t large[BRY_CONFIG_LIMIT];
typedef struct { size_t emit; bool reject, corrupt; } source;
static bool chunks(void *context, bry_store_sink sink, void *target) {
  source *s = context;
  for (size_t at = 0; at < s->emit;) {
    size_t n = s->emit - at;
    if (n > 257) n = 257;
    if (!sink(target, large + at, n)) return false;
    at += n;
  }
  if (s->corrupt) flash[0x90000+24] ^= 1;
  return !s->reject;
}
int main(void) {
  memset(flash, 255, sizeof(flash));
  bry_store s = owner();
  assert(s.blank && !s.owned && !mutations);
  flash[0x7ffff] = 0;
  assert(!bry_store_probe(&s) && !s.owned);
  assert(!bry_store_save(&s, "x", 1) && !mutations);
  memset(flash, 255, sizeof(flash));
  s = owner();
  assert(bry_store_save(&s, "old", 4));
  memcpy(baseline, flash, sizeof(flash));
  size_t required = 0;
  unsigned prior = mutations;
  assert(bry_store_load(&s, NULL, 0, &required) && required == 4 &&
         mutations == prior);
  assert(!bry_store_load(&s, NULL, 1, &required));
  for (unsigned cut = 0; cut < 36; cut++) {
    memcpy(flash, baseline, sizeof(flash));
    budget = -1;
    s = owner();
    char out[32];
    size_t n;
    assert(bry_store_load(&s, out, sizeof(out), &n) && !strcmp(out, "old"));
    budget = cut;
    bool saved = bry_store_save(&s, "new-data", 9);
    budget = -1;
    bry_store reboot = owner();
    assert(bry_store_load(&reboot, out, sizeof(out), &n));
    assert(!strcmp(out, saved ? "new-data" : "old"));
    assert(!memcmp(flash + 0x1000, baseline + 0x1000, 0x7f000));
  }
  memcpy(flash, baseline, sizeof(flash));
  s = owner();
  char out[32];
  size_t n;
  assert(bry_store_load(&s, out, sizeof(out), &n));
  assert(bry_store_save(&s, "new", 4));
  flash[0x90000 + 24] ^= 1;
  s = owner();
  assert(bry_store_load(&s, out, sizeof(out), &n) && !strcmp(out, "old"));
  for (size_t i = 0; i < sizeof(large); i++) large[i] = (uint8_t)(i*17);
  static const unsigned cuts[] = {0, 1, 255, 256, 257, 258, 513, 514,
      BRY_CONFIG_LIMIT-1, BRY_CONFIG_LIMIT, BRY_CONFIG_LIMIT+1,
      BRY_CONFIG_LIMIT+23, BRY_CONFIG_LIMIT+24, BRY_CONFIG_LIMIT+25};
  for (unsigned i = 0; i < sizeof(cuts)/sizeof(cuts[0]); i++) {
    memcpy(flash, baseline, sizeof(flash)); budget = -1; s = owner();
    assert(bry_store_load(&s, out, sizeof(out), &n));
    source data = {sizeof(large), false, false}; budget = cuts[i];
    bool saved = bry_store_save_stream(&s, sizeof(large), chunks, &data);
    budget = -1; bry_store reboot = owner();
    static uint8_t loaded[BRY_CONFIG_LIMIT];
    assert(bry_store_load(&reboot, loaded, sizeof(loaded), &n));
    assert(saved ? n == sizeof(large) && !memcmp(loaded, large, n) :
                   n == 4 && !memcmp(loaded, "old", 4));
    assert(!memcmp(flash+0x1000, baseline+0x1000, 0x7f000));
  }
  for (unsigned failure = 0; failure < 4; failure++) {
    memcpy(flash, baseline, sizeof(flash)); s = owner();
    assert(bry_store_load(&s, out, sizeof(out), &n));
    source data = {failure == 0 ? sizeof(large)-1 : sizeof(large),
                   failure == 2, failure == 3};
    size_t expected = failure == 1 ? sizeof(large)-1 : sizeof(large);
    assert(!bry_store_save_stream(&s, expected, chunks, &data));
    bry_store reboot = owner();
    assert(bry_store_load(&reboot, out, sizeof(out), &n) && !strcmp(out, "old"));
  }
  puts("storage: blank ownership, occupied refusal, 36 interrupted commits, "
       "14 interrupted 48KiB streams, short/long/rejected/corrupt producers, "
       "CRC fallback, image-region preservation");
}

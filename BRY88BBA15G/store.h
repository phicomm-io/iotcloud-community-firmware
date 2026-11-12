#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define BRY_STORE_SIZE 0x100000u
#define BRY_CONFIG_LIMIT 0xc000u
#define BRY_IMAGE_HEADER 0x1000u
#define BRY_IMAGE_START 0x2000u
#define BRY_IMAGE_LIMIT 0x67000u
typedef struct {
  void *context;
  bool (*read)(void *, uint32_t, void *, size_t);
  bool (*write)(void *, uint32_t, const void *, size_t);
  bool (*erase)(void *, uint32_t, size_t);
  bool owned, blank;
  uint32_t generation, slot;
} bry_store;
bool bry_store_probe(bry_store *);
bool bry_store_claim(bry_store *);
/* out=NULL, capacity=0 validates records and returns the exact payload size. */
bool bry_store_load(bry_store *, void *, size_t, size_t *);
bool bry_store_save(bry_store *, const void *, size_t);
/* The producer writes exactly length bytes to the sink. The inactive slot is
 * committed only after the complete stream and its readback CRC succeed. */
typedef bool (*bry_store_sink)(void *, const void *, size_t);
typedef bool (*bry_store_producer)(void *, bry_store_sink, void *);
bool bry_store_save_stream(bry_store *, size_t, bry_store_producer, void *);

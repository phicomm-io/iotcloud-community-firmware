#ifndef PHICOMM_SIGNED_OTA_H
#define PHICOMM_SIGNED_OTA_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define OTA_ENVELOPE_SIZE 192u
/* SDK nano printf need not support %llu. Preserve all 64 bits in JSON strings. */
static inline void ota_counter_decimal(uint64_t value, char out[21])
{
    char reverse[20]; unsigned n=0,i;
    do {reverse[n++]=(char)('0'+value%10);value/=10;} while(value);
    for(i=0;i<n;i++)out[i]=reverse[n-1-i];out[n]=0;
}

/* Callbacks must not activate firmware. Activation remains the caller's final step. */
typedef struct {
    bool (*erase)(void *);
    bool (*write)(void *, uint32_t, const uint8_t *, size_t);
    bool (*read)(void *, uint32_t, uint8_t *, size_t);
    void *context;
} signed_ota_io;
typedef struct {
    uint8_t envelope[OTA_ENVELOPE_SIZE];
    uint32_t total, received, image_length, written, maximum;
    uint16_t model, hardware;
    bool authenticated, failed;
    signed_ota_io io;
} signed_ota;
bool signed_ota_begin(signed_ota *, uint32_t total, uint32_t maximum,
                      uint16_t model, uint16_t hardware, signed_ota_io);
bool signed_ota_write(signed_ota *, const uint8_t *, size_t);
bool signed_ota_finish(signed_ota *);
#endif

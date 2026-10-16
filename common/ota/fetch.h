#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef bool (*ota_fetch_begin)(void *, uint32_t);
typedef bool (*ota_fetch_write)(void *, uint32_t, const uint8_t *, size_t);
/* One synchronous, bounded transfer; caller owns job reservation/lifecycle. */
bool ota_fetch(const char *url, uint32_t limit, ota_fetch_begin begin,
               ota_fetch_write write, void *ctx, const char **error);
bool ota_url_valid(const char *url);

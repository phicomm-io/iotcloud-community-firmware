/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PH_OTA_GUARD_H
#define PH_OTA_GUARD_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef struct { uint32_t expected, received; bool failed; } ph_ota_guard;
/* Native bootloader verifies RBL CRC/decompression. Reject bad HTTP framing
 * before writing and prevent an incomplete download from rebooting the device. */
static inline bool ph_ota_chunk(ph_ota_guard *g, int status, uint32_t expected,
                                const unsigned char *data, size_t n, uint32_t limit) {
    static const unsigned char magic[4]={'R','B','L',0};
    if(g->failed || status!=200 || expected<96 || expected>limit ||
       (g->expected && expected!=g->expected) || g->received>expected || n>expected-g->received) {
        g->failed=true;return false;
    }
    for(size_t i=0;i<n && g->received+i<4;++i)if(data[i]!=magic[g->received+i]) {
        g->failed=true;return false;
    }
    g->expected=expected;g->received+=(uint32_t)n;return true;
}
static inline bool ph_ota_complete(const ph_ota_guard *g) {
    return !g->failed && g->expected>=96 && g->received==g->expected;
}
#endif

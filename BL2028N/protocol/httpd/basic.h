/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PH_HTTP_BASIC_H
#define PH_HTTP_BASIC_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Shared by the HTTP parser API and local-auth core without a socket/parser
 * link dependency. Match the canonical encoding of the exact credentials. */
static inline bool ph_http_basic_match(const char *authorization, const char *user, const char *password)
{
    if (!password[0]) return true;
    if (!authorization) return false;
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char raw[72], expected[104] = "Basic ";
    size_t u = strlen(user), p = strlen(password);
    if (u + p + 1 >= sizeof(raw)) return false;
    memcpy(raw, user, u); raw[u] = ':'; memcpy(raw + u + 1, password, p);
    size_t n = u + p + 1, pos = 6;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)(unsigned char)raw[i] << 16;
        if (i+1 < n) v |= (uint32_t)(unsigned char)raw[i+1] << 8;
        if (i+2 < n) v |= (unsigned char)raw[i+2];
        expected[pos++] = alphabet[v>>18]; expected[pos++] = alphabet[(v>>12)&63];
        expected[pos++] = i+1<n ? alphabet[(v>>6)&63] : '=';
        expected[pos++] = i+2<n ? alphabet[v&63] : '=';
    }
    expected[pos] = 0;
    if (strlen(authorization) != pos) return false;
    unsigned diff = 0;
    for (size_t i=0; i<pos; ++i) diff |= (unsigned char)authorization[i] ^ (unsigned char)expected[i];
    return diff == 0;
}
#endif

#pragma once
#include <string.h>
/* Additional origins must be literal, explicitly trusted origins at build time:
 * -DPHICOMM_CORS_EXTRA_ORIGINS='X("https://example.org")' */
#ifndef PHICOMM_CORS_EXTRA_ORIGINS
#define PHICOMM_CORS_EXTRA_ORIGINS
#endif
static inline const char *phicomm_cors_headers(const char *origin)
{
#define X(site) if (!strcmp(origin,site)) return "Access-Control-Allow-Origin: " site "\r\nVary: Origin\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Headers: Authorization, Content-Type\r\nAccess-Control-Allow-Private-Network: true\r\nAccess-Control-Max-Age: 600\r\n";
    X("https://mqtt.fjrcn.cn")
    X("https://mqtt.fjrcn.com")
    X("https://phicomm-io.github.io")
#ifdef PHICOMM_DEVELOPMENT
    X("http://localhost:3000")
    X("http://127.0.0.1:3000")
#endif
    PHICOMM_CORS_EXTRA_ORIGINS
#undef X
    return "";
}

#pragma once
#include <stdbool.h>
/* Fixed-size credential replacement; no allocation and no fail-open fallback. */
int httpd_auth_init(const char *user, const char *password);
bool tc1_http_authorized(const char *authorization);

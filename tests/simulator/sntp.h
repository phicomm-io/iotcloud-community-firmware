#pragma once
#include "mico.h"
typedef struct {uint32_t seconds,microseconds;} ntp_timestamp_t;
static inline int sntp_get_time(struct in_addr *a,ntp_timestamp_t *t){(void)a;t->seconds=time(NULL);t->microseconds=0;return 0;}

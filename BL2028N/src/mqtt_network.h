/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "os.h"
#include <stdlib.h>
#include <string.h>
/* Only the portable Paho protocol is reused. This is its Beken socket/timer
 * binding; no MiCO Network, TLS or RTOS implementation is linked. */
typedef struct { uint32_t deadline; } Timer;
typedef struct Network Network;
struct Network {
    int my_socket;
    int (*mqttread)(Network *, unsigned char *, int, int);
    int (*mqttwrite)(Network *, unsigned char *, int, int);
    void (*disconnect)(Network *);
};
typedef struct { unsigned unused; } ph_network_options;
void InitTimer(Timer *);
char expired(Timer *);
void countdown_ms(Timer *, unsigned);
void countdown(Timer *, unsigned);
int left_ms(Timer *);
int ph_network_open(Network *, const char *, unsigned, ph_network_options);

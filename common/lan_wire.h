/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "typed_wire.h"
/* Native LAN v3. Integer measurements have fixed declared units:
 * temperature milli-Celsius, humidity milli-percent, PM25 milli-ug/m3,
 * formaldehyde micro-ug/m3 (mg/m3 * 1000000), CO2 ppm. */
typedef enum { LAN_READ=1,LAN_PROBE,LAN_SAMPLE,LAN_PONG } lan_operation;
typedef struct {
    uint32_t boot,sequence;
    int32_t value[5];
    uint32_t age_ms[5];
    uint8_t fields;
    bool co2_supported;
} lan_sample;
typedef struct {
    lan_operation op;
    char src[13],dst[13];
    uint8_t token[16];
    uint32_t boot,id;
    lan_sample sample;
} lan_packet;
/* Decode makes an independent typed copy. No borrowed pointers escape. */
bool lan_decode(const void *,size_t,lan_packet *);
/* Accept an already validated synchronous borrowed view; output still owns data. */
bool lan_decode_value(const ph_value *,lan_packet *);
bool lan_encode(ph_writer *,const lan_packet *);

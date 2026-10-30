/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "../protocol/typed_wire.h"
#define PH_METER_FRESH_MS 5000u
typedef struct {
    unsigned char buffer[24]; unsigned used, fields;
    uint32_t byte_at, field_at[3], frame_at, pulse_at, pulse;
    uint32_t frames, bad_frames, discontinuities;
    bool pulse_ready;
    double raw[3], value[3], energy_kwh;
} ph_meter;
void ph_meter_init(ph_meter *, double energy_kwh);
bool ph_meter_byte(ph_meter *, unsigned char, uint32_t now, const double coefficients[3]);
void ph_meter_write(const ph_meter *, ph_writer *, uint32_t now, bool calibrated);
unsigned ph_meter_fields(const ph_meter *, uint32_t now);

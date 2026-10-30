/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "../protocol/typed_wire.h"
#define PH_METER_FRESH_MS 5000u
/* S30-V1B stock 2M.1220 meter constants, not per-unit calibration.
 * See research/stock-meter-coefficients.md for binary evidence. */
#define PH_METER_DEFAULT_VOLTAGE_COEFFICIENT 1.894288
#define PH_METER_DEFAULT_CURRENT_COEFFICIENT 1.0102
#define PH_METER_DEFAULT_POWER_COEFFICIENT 1.88835008906368
typedef struct {
    unsigned char buffer[24]; unsigned used, fields;
    uint32_t byte_at, field_at[3], frame_at, pulse_at, pulse;
    uint32_t received_bytes, frames, bad_frames, discontinuities;
    bool pulse_ready;
    double raw[3], value[3], energy_kwh;
} ph_meter;
void ph_meter_init(ph_meter *, double energy_kwh);
bool ph_meter_byte(ph_meter *, unsigned char, uint32_t now, const double coefficients[3]);
void ph_meter_write(const ph_meter *, ph_writer *, uint32_t now, bool calibrated);
unsigned ph_meter_fields(const ph_meter *, uint32_t now);

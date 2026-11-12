/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "core.h"
#include "../typed_wire.h"
/* Only a short-lock POD copy; formatting never holds the queue mutex. */
void ph_mqtt_snapshot(ph_mqtt_diagnostics *);
void ph_mqtt_diag_write_mp(ph_writer *, bool available);

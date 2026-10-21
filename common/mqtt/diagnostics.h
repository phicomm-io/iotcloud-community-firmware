/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "core.h"
/* Only a short-lock POD copy; formatting never holds the queue mutex. */
void ph_mqtt_snapshot(ph_mqtt_diagnostics *);
size_t ph_mqtt_diag_append(char *,size_t,size_t,bool);

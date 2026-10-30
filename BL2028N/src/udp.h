/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "device.h"
typedef struct { char peer[13], token[33]; uint32_t boot, last, retired[4]; bool active; } ph_udp_peer;
typedef struct { ph_udp_peer peers[PH_AUTH_PAIR_SLOTS+PH_AUTH_MQTT_SLOTS]; } ph_udp_state;
/* Caller holds the shared device/auth mutex. Zero means silent rejection. */
size_t ph_udp_handle(ph_udp_state *, ph_device *, ph_auth_state *, const void *, size_t, void *, size_t);

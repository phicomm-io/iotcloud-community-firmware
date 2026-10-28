/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "phicomm_features.h"
#include "typed_wire.h"
/* Decode into a working copy; the caller commits only after validation. */
bool feature_config_decode_mp(feature_config_t *,const char *,const ph_value *);
bool feature_config_encode_mp(const feature_config_t *,const char *,ph_writer *);
bool feature_mp(const char *,const ph_value *,ph_writer *,const char **);

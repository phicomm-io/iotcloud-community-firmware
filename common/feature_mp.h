/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "phicomm_features.h"
#include "typed_wire.h"
/* Decode into a working copy; the caller commits only after validation. */
bool feature_config_decode_mp(feature_config_t *,const char *,const ph_value *);
bool feature_config_encode_mp(const feature_config_t *,const char *,ph_writer *);
bool feature_mp(const char *,const ph_value *,ph_writer *,const char **);
bool feature_domain_get_mp(const char *,ph_writer *);
int feature_domain_set_mp(const char *,const ph_value *,ph_writer *);
bool feature_ntp_set_mp(const char *,const char **);
int feature_light_select_mp(unsigned,ph_writer *);
int feature_fan_command_mp(const ph_value *);
bool feature_control_mp(const char *,const ph_value *,const char **);

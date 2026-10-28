/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "typed_wire.h"
/* Start copies every argument. Passwords remain RAM-only and are erased after
 * pairing terminates. accepted means queued, not durable pairing completion. */
bool feature_lan_pair_start(const char *,const char *,const char *,unsigned,const char **);
bool feature_lan_pair_status(const char *,ph_writer *);

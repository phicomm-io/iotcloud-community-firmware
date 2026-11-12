#pragma once
#include "typed_wire.h"
/* Local UDP delivery never broadcasts the request back into its own group. */
bool feature_control_mp(const char *kind,const ph_value *params,const char **code);
bool a1_control_mp(const char *kind,const ph_value *params,bool local);

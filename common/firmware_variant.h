#pragma once
/* Legacy JSON adapter; binary producers include firmware_variant_defs.h. */
#include "firmware_variant_defs.h"
static inline void phicomm_variant_json(json_object *o)
{
#define STATUS_INT(k,v) json_build_object_add(o,k,json_object_new_int(v))
#define STATUS_BOOL(k,v) json_build_object_add(o,k,json_object_new_boolean(v))
#define STATUS_STRING(k,v) json_build_object_add(o,k,json_object_new_string(v))
#include "variant_fields.inc"
#undef STATUS_INT
#undef STATUS_BOOL
#undef STATUS_STRING
}

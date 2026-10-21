#pragma once
#ifndef PHICOMM_FULL_RESOURCES
#define PHICOMM_FULL_RESOURCES 1
#endif
#if PHICOMM_FULL_RESOURCES
#define PHICOMM_FIRMWARE_VARIANT "full"
#define PHICOMM_OTA_VARIANT_PATH ""
#else
#define PHICOMM_FIRMWARE_VARIANT "no-frontend-resources"
#define PHICOMM_OTA_VARIANT_PATH "/no-frontend-resources"
#endif
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

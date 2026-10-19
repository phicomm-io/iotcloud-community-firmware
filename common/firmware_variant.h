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
    json_build_object_add(o,"api_version",json_object_new_int(1));
    json_build_object_add(o,"firmware_variant",json_object_new_string(PHICOMM_FIRMWARE_VARIANT));
    json_build_object_add(o,"full_resources",json_object_new_boolean(PHICOMM_FULL_RESOURCES));
    json_build_object_add(o,"bootstrap_ui",json_object_new_boolean(!PHICOMM_FULL_RESOURCES));
}

#pragma once
#ifndef PHICOMM_FULL_RESOURCES
#define PHICOMM_FULL_RESOURCES 1
#endif
#if defined(A1_FRESH_AIR) && PHICOMM_FULL_RESOURCES
#define PHICOMM_FIRMWARE_VARIANT "fresh-air"
#define PHICOMM_OTA_VARIANT_PATH "/fresh-air"
#elif defined(A1_FRESH_AIR)
#define PHICOMM_FIRMWARE_VARIANT "fresh-air-no-frontend-resources"
#define PHICOMM_OTA_VARIANT_PATH "/fresh-air-no-frontend-resources"
#elif PHICOMM_FULL_RESOURCES
#define PHICOMM_FIRMWARE_VARIANT "full"
#define PHICOMM_OTA_VARIANT_PATH ""
#else
#define PHICOMM_FIRMWARE_VARIANT "no-frontend-resources"
#define PHICOMM_OTA_VARIANT_PATH "/no-frontend-resources"
#endif
#ifdef A1_FRESH_AIR
#define PHICOMM_LEGACY_FULL_VARIANT 0
#else
#define PHICOMM_LEGACY_FULL_VARIANT PHICOMM_FULL_RESOURCES
#endif

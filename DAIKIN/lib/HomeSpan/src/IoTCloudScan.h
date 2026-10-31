// SPDX-License-Identifier: MIT
#pragma once
#include <stdio.h>

// Every HomeSpan input format is an integer or character conversion. Newlib's
// siscanf preserves those sscanf conversions and shares the scanner already
// linked by ESP-IDF, avoiding a second scanner with unused float conversions.
// Host fixtures use their libc's equivalent non-floating formats.
#ifdef ARDUINO_ARCH_ESP32
#define IOTCLOUD_HAP_SCAN siscanf
#else
#define IOTCLOUD_HAP_SCAN sscanf
#endif

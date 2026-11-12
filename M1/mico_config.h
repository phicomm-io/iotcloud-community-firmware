#pragma once

#define APP_INFO "Phicomm M1 community firmware"
#include "../common/generated/build_version.h"
#define FIRMWARE_REVISION FIRMWARE_VERSION
#define MANUFACTURER "Phicomm Community"
#define SERIAL_NUMBER "M1"
#define PROTOCOL "openiot.m1.local"
/* Foreign factory layout initialization nests the application and defaults
 * frames (at least 4296 bytes in GCC 5.4.1), before SDK/RTOS callees. */
#define MICO_DEFAULT_APPLICATION_STACK_SIZE 8192
#define MICO_WLAN_CONFIG_MODE CONFIG_MODE_EASYLINK_WITH_SOFTAP
#define EasyLink_TimeOut 60000
#define EasyLink_ConnectWlan_Timeout 20000
/* Application owns Wi-Fi and HTTP. Do not enable SDK autoconf/config server. */

#define MICO_CONFIG_SERVER_PORT 8000 /* Compiled SDK code; service stays disabled. */

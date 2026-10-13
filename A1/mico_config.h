#pragma once

#define APP_INFO "Phicomm A1 community firmware"
#define FIRMWARE_REVISION "1.0.0"
#define MANUFACTURER "Phicomm Community"
#define SERIAL_NUMBER "A1"
#define PROTOCOL "phicomm.a1.local"
#define MICO_DEFAULT_APPLICATION_STACK_SIZE 4096
#define MICO_WLAN_CONFIG_MODE CONFIG_MODE_EASYLINK_WITH_SOFTAP
#define EasyLink_TimeOut 60000
#define EasyLink_ConnectWlan_Timeout 20000
/* Application owns Wi-Fi and HTTP. Do not enable SDK autoconf/config server. */

#define MICO_CONFIG_SERVER_PORT 8000 /* Compiled SDK code; service stays disabled. */

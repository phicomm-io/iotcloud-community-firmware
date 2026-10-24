###############################################################################
#
#  The MIT License
#  Copyright (c) 2016 MXCHIP Inc.
#
#  Permission is hereby granted, free of charge, to any person obtaining a copy
#  of this software and associated documentation files (the "Software"), to deal
#  in the Software without restriction, including without limitation the rights
#  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
#  copies of the Software, and to permit persons to whom the Software is furnished
#  to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice shall be included in
#  all copies or substantial portions of the Software.
#
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
#  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
#  WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
#  IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
###############################################################################


NAME := App_TC1

$(NAME)_SOURCES := http_trace.c\
                   main.c status_guard.c\
                   service.c\
                   ota_signature.c\
                   ota_crypto.c\
                   ota_server/signed_adapter.c\
				   ota_server/ota_server.c\
				   user_wifi.c\
				   user_gpio.c\
				   time_server/user_rtc.c\
				   mqtt_server/user_mqtt_client.c\
				   ota_server/user_ota.c\
				   user_power.c\
				   timed_task/timed_task.c\
				   http_server/web_log.c\
				   http_server/app_httpd.c\
                   http_server/auth.c

$(NAME)_COMPONENTS := protocols/SNTP\
					protocols/mqtt\
					utilities/json_c\
					utilities/url\
					phicomm_httpd phicomm_mqtt

GLOBAL_DEFINES += DEV_MODEL=\"PHICOMM_TC1_A1\"

# JSON decimal parsing uses the private checked numeric component.

$(NAME)_SOURCES += ota_fetch.c ota_mode.c
GLOBAL_DEFINES += PHICOMM_DEVICE_OTA=1
GLOBAL_CFLAGS += -fstack-usage

# Each variant has its own generated resource directory and build workspace.
PHICOMM_FULL_RESOURCES ?= 1
GLOBAL_DEFINES += PHICOMM_FULL_RESOURCES=$(PHICOMM_FULL_RESOURCES)
$(NAME)_CFLAGS += -I$(PHICOMM_WEB_OUTPUT_DIR)

GLOBAL_DEFINES += PHICOMM_HTTP_TRACE=1

GLOBAL_DEFINES += PH_HTTP_CLIENT_SLOTS=4

GLOBAL_DEFINES += PHICOMM_BEMFA=1 BEMFA_TC1=1

GLOBAL_DEFINES += PHICOMM_STATUS_MODEL=2

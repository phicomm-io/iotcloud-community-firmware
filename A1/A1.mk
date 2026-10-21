NAME := App_A1
$(NAME)_SOURCES := main.c watchdog.c service.c features.c protocol.c control.c hardware.c network.c mqtt.c http.c ota.c ota_signature.c ota_crypto.c
$(NAME)_INCLUDES := . generated
$(NAME)_CFLAGS += -fstack-usage
$(NAME)_COMPONENTS := protocols/SNTP protocols/mqtt utilities/json_c phicomm_httpd
GLOBAL_DEFINES += DEV_MODEL=\"PHICOMM_A1_EMW3080\" DISABLE_MICO_BLUENRG=1

GLOBAL_DEFINES += MAX_MESSAGE_HANDLERS=12 PHICOMM_APPLICATION_WATCHDOG=1

# json-c parses decimal JSON numbers with sscanf("%lf").
GLOBAL_LDFLAGS += -u _scanf_float

$(NAME)_SOURCES += ota_fetch.c ota_mode.c
$(NAME)_INCLUDES += generated/tls-include
$(NAME)_PREBUILT_LIBRARY := generated/libbearssl.a
GLOBAL_DEFINES += PHICOMM_DEVICE_OTA=1

# Each variant has its own generated resource directory and build workspace.
PHICOMM_FULL_RESOURCES ?= 1
GLOBAL_DEFINES += PHICOMM_FULL_RESOURCES=$(PHICOMM_FULL_RESOURCES)
$(NAME)_CFLAGS += -I$(PHICOMM_WEB_OUTPUT_DIR)

GLOBAL_DEFINES += PH_HTTP_CLIENT_SLOTS=3
GLOBAL_DEFINES += PH_HTTP_SCALAR_BYTES=1280

NAME := App_M1
$(NAME)_SOURCES := main.c watchdog.c wire.c service.c features.c protocol.c scd40.c uart.c network.c mqtt.c mqtt_transport.c http.c ota.c ota_signature.c ota_crypto.c
$(NAME)_INCLUDES := . generated
$(NAME)_CFLAGS += -fstack-usage
# Capture SDK SHAUtils frames required by the LAN stack gate as well.
GLOBAL_CFLAGS += -fstack-usage
$(NAME)_SOURCES += mcu.c barometer.c bmp3_vendor/bmp3.c
$(NAME)_SOURCES += co2_source.c display_source.c
GLOBAL_DEFINES += BMP3_64BIT_COMPENSATION=1
$(NAME)_COMPONENTS := protocols/SNTP protocols/mqtt phicomm_httpd phicomm_mqtt
GLOBAL_DEFINES += DEV_MODEL=\"PHICOMM_M1_EMW3080\" DISABLE_MICO_BLUENRG=1

# Both optimized SDK modes are release builds, including the default with logs.
ifneq ($(filter release release_log,$(BUILD_TYPE)),)
GLOBAL_DEFINES += PHICOMM_EMW3080_RELEASE_MEMORY=1
endif


GLOBAL_DEFINES += MAX_MESSAGE_HANDLERS=12 PHICOMM_APPLICATION_WATCHDOG=1


$(NAME)_SOURCES += ota_fetch.c ota_mode.c
GLOBAL_DEFINES += PHICOMM_DEVICE_OTA=1

# Each variant has its own generated resource directory and build workspace.
PHICOMM_FULL_RESOURCES ?= 1
GLOBAL_DEFINES += PHICOMM_FULL_RESOURCES=$(PHICOMM_FULL_RESOURCES)
$(NAME)_CFLAGS += -I$(PHICOMM_WEB_OUTPUT_DIR)

GLOBAL_DEFINES += PH_HTTP_CLIENT_SLOTS=4
GLOBAL_DEFINES += PH_HTTP_SCALAR_BYTES=1280

GLOBAL_DEFINES += PHICOMM_STATUS_MODEL=1

# Share stateless HA codecs while retaining per-owner input buffers.
GLOBAL_DEFINES += PHICOMM_SHARED_HA_CODEC=1

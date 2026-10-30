NAME := App_A1
$(NAME)_SOURCES := main.c watchdog.c wire.c service.c features.c protocol.c control.c hardware.c network.c mqtt.c http.c ota.c ota_signature.c ota_crypto.c
$(NAME)_INCLUDES := . generated
# Keep control callees shared and bound stack growth in the fixed A1 slot.
$(NAME)_CFLAGS += -fstack-usage -fno-inline-small-functions -fconserve-stack
# Capture SDK SHAUtils frames required by the LAN stack gate as well.
GLOBAL_CFLAGS += -fstack-usage
$(NAME)_COMPONENTS := protocols/SNTP protocols/mqtt utilities/json_c phicomm_httpd phicomm_mqtt
GLOBAL_DEFINES += DEV_MODEL=\"PHICOMM_A1_EMW3080\" DISABLE_MICO_BLUENRG=1

# Both optimized SDK modes are release builds, including the default with logs.
ifneq ($(filter release release_log,$(BUILD_TYPE)),)
GLOBAL_DEFINES += PHICOMM_EMW3080_RELEASE_MEMORY=1
endif

GLOBAL_DEFINES += MAX_MESSAGE_HANDLERS=12 PHICOMM_APPLICATION_WATCHDOG=1

# JSON decimal parsing uses the private checked numeric component.

$(NAME)_SOURCES += ota_fetch.c ota_mode.c
GLOBAL_DEFINES += PHICOMM_DEVICE_OTA=1

# Each variant has its own generated resource directory and build workspace.
PHICOMM_FULL_RESOURCES ?= 1
GLOBAL_DEFINES += PHICOMM_FULL_RESOURCES=$(PHICOMM_FULL_RESOURCES)
$(NAME)_CFLAGS += -I$(PHICOMM_WEB_OUTPUT_DIR)

GLOBAL_DEFINES += PH_HTTP_CLIENT_SLOTS=4
GLOBAL_DEFINES += PH_HTTP_SCALAR_BYTES=1280

GLOBAL_DEFINES += PHICOMM_BEMFA=1

GLOBAL_DEFINES += PHICOMM_STATUS_MODEL=0

$(NAME)_SOURCES += fresh_air_defaults.c
ifeq ($(A1_FRESH_AIR),1)
GLOBAL_DEFINES += A1_FRESH_AIR=1
$(NAME)_SOURCES += fresh_air.c
endif

# Share stateless HA codecs while retaining per-owner input buffers.
GLOBAL_DEFINES += PHICOMM_SHARED_HA_CODEC=1

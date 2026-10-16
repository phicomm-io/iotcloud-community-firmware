NAME := App_A1
$(NAME)_SOURCES := main.c watchdog.c service.c features.c protocol.c control.c hardware.c network.c mqtt.c http.c ota.c ota_signature.c ota_crypto.c
$(NAME)_INCLUDES := . generated
$(NAME)_CFLAGS += -fstack-usage
$(NAME)_COMPONENTS := protocols/SNTP protocols/mqtt utilities/json_c
GLOBAL_DEFINES += DEV_MODEL=\"PHICOMM_A1_EMW3080\" DISABLE_MICO_BLUENRG=1

GLOBAL_DEFINES += MAX_MESSAGE_HANDLERS=12 PHICOMM_APPLICATION_WATCHDOG=1

# json-c parses decimal JSON numbers with sscanf("%lf").
GLOBAL_LDFLAGS += -u _scanf_float

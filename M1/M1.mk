NAME := App_M1
$(NAME)_SOURCES := main.c service.c features.c protocol.c uart.c network.c mqtt.c mqtt_transport.c http.c ota.c ota_signature.c ota_crypto.c
$(NAME)_INCLUDES := . generated
$(NAME)_COMPONENTS := protocols/SNTP protocols/SNTP protocols/mqtt utilities/json_c
GLOBAL_DEFINES += DEV_MODEL=\"PHICOMM_M1_EMW3080\" DISABLE_MICO_BLUENRG=1


GLOBAL_DEFINES += MAX_MESSAGE_HANDLERS=12

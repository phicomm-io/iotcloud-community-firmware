NAME := App_A1
$(NAME)_SOURCES := main.c features.c protocol.c control.c hardware.c network.c mqtt.c http.c ota.c
$(NAME)_INCLUDES := . generated
$(NAME)_COMPONENTS := protocols/SNTP protocols/mqtt utilities/json_c
GLOBAL_DEFINES += DEV_MODEL=\"PHICOMM_A1_EMW3080\" DISABLE_MICO_BLUENRG=1

GLOBAL_DEFINES += MAX_MESSAGE_HANDLERS=12

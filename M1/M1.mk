NAME := App_M1
$(NAME)_SOURCES := main.c protocol.c uart.c network.c mqtt.c http.c ota.c
$(NAME)_INCLUDES := . generated
$(NAME)_COMPONENTS := protocols/SNTP protocols/mqtt utilities/json_c
GLOBAL_DEFINES += DEV_MODEL=\"PHICOMM_M1_EMW3080\" DISABLE_MICO_BLUENRG=1


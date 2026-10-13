NAME := App_A1
$(NAME)_SOURCES := main.c protocol.c control.c hardware.c network.c mqtt.c http.c ota.c
$(NAME)_INCLUDES := . generated
$(NAME)_COMPONENTS := protocols/mqtt utilities/json_c
GLOBAL_DEFINES += DEV_MODEL=\"PHICOMM_A1_EMW3080\" DISABLE_MICO_BLUENRG=1

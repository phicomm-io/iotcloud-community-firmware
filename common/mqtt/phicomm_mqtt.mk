NAME := phicomm_mqtt
$(NAME)_SOURCES := core.c client.c service.c session.c diagnostics.c
GLOBAL_INCLUDES += .
$(NAME)_CFLAGS += -fstack-usage

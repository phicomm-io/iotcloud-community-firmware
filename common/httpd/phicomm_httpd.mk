NAME := phicomm_httpd
$(NAME)_SOURCES := core.c parser.c mico_port.c product.c json_owned.c device_status.c
GLOBAL_INCLUDES += .
$(NAME)_CFLAGS += -fstack-usage

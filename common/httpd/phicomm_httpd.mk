NAME := phicomm_httpd
$(NAME)_SOURCES := core.c parser.c mico_port.c product.c json_owned.c
GLOBAL_INCLUDES += .
$(NAME)_CFLAGS += -fstack-usage

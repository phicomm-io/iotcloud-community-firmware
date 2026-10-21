#pragma once
#ifndef PHICOMM_STRONG_WIRE_FIXTURE
/* Portable fixtures model dispatch separately; strong wire APIs override these. */
#include "common/json_owned.h"
extern json_object *service_command(json_object *) __attribute__((weak));
extern json_object *service_query(const char *) __attribute__((weak));
__attribute__((weak)) struct printbuf *service_command_wire(json_object *req){return service_command?json_owned_to_wire(service_command(req)):NULL;}
__attribute__((weak)) struct printbuf *service_query_wire(const char *action){return service_query?json_owned_to_wire(service_query(action)):NULL;}
#endif

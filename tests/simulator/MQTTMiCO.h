#pragma once
#include "mico.h"
typedef struct { uint64_t end_time; } Timer;
typedef struct Network Network;
struct Network { int my_socket; int (*mqttread)(Network *,unsigned char *,int,int); int (*mqttwrite)(Network *,unsigned char *,int,int); void (*disconnect)(Network *); };
typedef struct { bool ssl_enable; } ssl_opts;
char expired(Timer *); void countdown_ms(Timer *,unsigned); void countdown(Timer *,unsigned); int left_ms(Timer *); void InitTimer(Timer *);
int NewNetwork(Network *,char *,int,ssl_opts);

#pragma once
/* Shared device business now consumes only borrowed MessagePack views. */
#include "service_mp.h"
void service_memory_mqtt(unsigned pending,bool overflow,uint32_t delay_ms);

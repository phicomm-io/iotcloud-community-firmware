#pragma once
#include "mico.h"
typedef struct { char name[64]; } micoSystemConfig_t;
typedef struct { micoSystemConfig_t micoSystemConfig; } system_config_t;
typedef system_config_t mico_Context_t;

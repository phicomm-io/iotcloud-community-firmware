#pragma once
#include "../../common/phicomm_features.h"
#include "json.h"
#include "mico.h"
#include "../../M1/scd40.h"
typedef struct {
  int temperature_deci_c, humidity_deci_pct;
  unsigned pm25_ug_m3, hcho_ug_m3, sequence, updated_ms;
  bool received;
} m1_sample_t;
typedef struct {
  bool wifi_connected, mqtt_connected;
  char ip[16];
  m1_sample_t sample;
  scd40_state_t scd40;
} m1_state_t;
extern char m1_id[13];
void m1_state_get(m1_state_t *);

extern mico_mutex_t m1_lock;
extern m1_state_t m1_state;

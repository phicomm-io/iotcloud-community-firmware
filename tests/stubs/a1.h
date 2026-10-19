#pragma once
#include "../../common/phicomm_features.h"
#include "json.h"
#include "mico.h"
typedef struct {
  char name[64];
  bool fan_on, led_enabled, child_lock, led_always_on;
  unsigned speed, brightness, red, green, blue;
} a1_config_t;
typedef struct {
  bool wifi_connected, mqtt_connected, led_visible;
  char ip[16];
} a1_state_t;
extern char a1_id[13];
void a1_config_get(a1_config_t *);
void a1_state_get(a1_state_t *);
bool a1_control_json(const char *, json_object *);

bool a1_control_local(const char *, json_object *);

extern mico_mutex_t a1_lock;
extern a1_state_t a1_state;

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "service.h"
#define AUTOMATION_MAGIC 0x41555432u
#ifdef AUTOMATION_TC1
#define AUTOMATION_TASKS 128
#else
#define AUTOMATION_TASKS 8
#endif
enum { AUTO_NONE, AUTO_OUTLET, AUTO_FAN, AUTO_LIGHT, AUTO_CHILD_LOCK, AUTO_INDICATOR, AUTO_DISPLAY, AUTO_REBOOT };
/* Append action IDs: existing persisted actions retain their meaning. */
enum { AUTO_LIGHT_EFFECT = AUTO_REBOOT + 1, AUTO_FILTER_RESET, AUTO_LIGHT_ALWAYS, AUTO_FAN_MODE, AUTO_FAN_MODE_TOGGLE, AUTO_FAN_PANEL, AUTO_WAKE_ALARM, AUTO_ACTION_MAX = AUTO_WAKE_ALARM };
#if defined(FEATURE_DEVICE_A1) || defined(HOST_A1)
#define AUTOMATION_KEYS 3
#else
#define AUTOMATION_KEYS 2
#endif
#define AUTOMATION_BUTTON_SLOTS (AUTOMATION_KEYS * 34)
enum { AUTO_WAITING, AUTO_EXECUTED, AUTO_MISSED, AUTO_BUSY, AUTO_MIGRATED_DISABLED, AUTO_CANCELLED };
/* once: 0=weekly, 1=wall-clock once, 2=volatile countdown (at=duration seconds).
 * The fixed Flash layout stays unchanged. A reboot cancels active countdowns. */
#define AUTOMATION_COUNTDOWN_MAX 86400u
/* Stable fixed-width Flash ABI: never stores pointers or JSON. */
typedef struct { uint8_t kind, target, value, speed, red, green, blue, reserved; } automation_action_t;
typedef struct {
    uint16_t id, minute;
    uint8_t enabled, days, once, state;
    uint32_t at, last;
    automation_action_t action;
} automation_task_t;
/* On-flash v4/v5 prefix remains fixed at two keys, including on A1. */
typedef struct {
    uint32_t magic, revision;
    uint16_t next_id;
    int16_t timezone;
    automation_task_t tasks[AUTOMATION_TASKS];
    automation_action_t buttons[2][34];
} automation_storage_t;
#if defined(FEATURE_DEVICE_A1) || defined(HOST_A1)
/* Working copy: A1 storage adapters append the third map after the v5 filter. */
typedef struct {
    uint32_t magic, revision;
    uint16_t next_id;
    int16_t timezone;
    automation_task_t tasks[AUTOMATION_TASKS];
    automation_action_t buttons[3][34];
} automation_config_t;
#else
typedef automation_storage_t automation_config_t;
#endif
void automation_defaults(automation_config_t *);
void automation_load(automation_config_t *);
bool automation_save(const automation_config_t *);
/* Uses same transport dispatcher; unknown action leaves handled=false. */
json_object *automation_command(const char *,json_object *,const char **,bool *handled);
void automation_poll(bool clock_valid);
void automation_button(unsigned key,unsigned clicks,unsigned seconds);
#ifdef FEATURE_DEVICE_A1
/* Product business entry; does not build a status or command JSON graph. */
bool a1_automation_execute(const automation_action_t *, bool button);
#endif
#ifdef AUTOMATION_TC1
/* Read only the selected map; execute without constructing JSON graphs. */
bool tc1_automation_load_button(unsigned key, unsigned slot, automation_action_t *);
bool tc1_automation_execute(const automation_action_t *, bool button);
#endif

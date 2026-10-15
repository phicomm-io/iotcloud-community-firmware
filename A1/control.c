#include "a1.h"
#include <ctype.h>

json_object *a1_json_parse(const char *data, size_t length)
{
    if (!length || length > A1_JSON_LIMIT || memchr(data, 0, length)) return NULL;
    struct json_tokener *tok = json_tokener_new();
    if (!tok) return NULL;
    json_object *o = json_tokener_parse_ex(tok, data, length);
    int consumed = tok->char_offset;
    bool valid = tok->err == json_tokener_success && o && json_object_is_type(o, json_type_object);
    while (consumed < (int)length && isspace((unsigned char)data[consumed])) consumed++;
    valid = valid && consumed == (int)length;
    json_tokener_free(tok);
    if (!valid) { if (o) json_object_put(o); return NULL; }
    /* json-c represents both missing fields and null as NULL. Reject explicit null. */
    json_object_object_foreach(o, key, value) {
        UNUSED_PARAMETER(key);
        if (!value) { json_object_put(o); return NULL; }
    }
    return o;
}

/* caller holds both mutexes; repeated commands wake LEDs without wearing Flash. */
static void apply(const a1_config_t *c)
{
    if (memcmp(c, a1_config, sizeof(*c))) { *a1_config = *c; a1_control_changed(); }
    else a1_state.led_until = mico_rtos_get_time() + 2000;
}

static bool get_bool(json_object *o, const char *key, uint8_t *out, bool *found)
{
    json_object *v = json_object_object_get(o, key);
    if (!v) return true;
    if (!json_object_is_type(v, json_type_boolean)) return false;
    *found = true; *out = json_object_get_boolean(v) ? 1 : 0; return true;
}
static bool get_byte(json_object *o, const char *key, uint8_t *out, int max, bool *found)
{
    json_object *v = json_object_object_get(o, key);
    if (!v) return true;
    if (!json_object_is_type(v, json_type_int)) return false;
    int n = json_object_get_int(v);
    if (n < 0 || n > max) return false;
    *found = true; *out = n; return true;
}

bool a1_control_local(const char *kind, json_object *o)
{
    if (!o || !json_object_is_type(o, json_type_object)) return false;
    mico_rtos_lock_mutex(&a1_config_lock);
    mico_rtos_lock_mutex(&a1_lock);
    a1_config_t c = *a1_config;
    bool valid = !a1_state.ota_active && !a1_state.reboot_pending, found = false;
    if (!strcmp(kind, "fan")) {
        uint8_t speed = c.speed;
        valid = valid && get_byte(o, "speed", &speed, 100, &found);
        if (json_object_object_get(o, "speed")) {
            c.fan_on = speed != 0; if (speed) c.speed = speed;
        }
        valid = valid && get_bool(o, "on", &c.fan_on, &found);
    } else if (!strcmp(kind, "light")) {
        valid = valid && get_bool(o, "enabled", &c.led_enabled, &found) &&
            get_bool(o, "always_on", &c.led_always_on, &found) && get_byte(o, "brightness", &c.brightness, 255, &found);
        json_object *color = json_object_object_get(o, "color");
        if (color) {
            bool r = false, g = false, b = false;
            valid = valid && json_object_is_type(color, json_type_object) &&
                get_byte(color, "r", &c.red, 255, &r) && get_byte(color, "g", &c.green, 255, &g) &&
                get_byte(color, "b", &c.blue, 255, &b) && r && g && b;
            found = true;
        }
        if (!json_object_object_get(o, "enabled") &&
            (color || json_object_object_get(o, "brightness"))) c.led_enabled = 1;
        /* HA JSON light uses state ON/OFF. Other fields preserve their values. */
        json_object *state = json_object_object_get(o, "state");
        if (state) {
            valid = valid && json_object_is_type(state, json_type_string);
            const char *s = json_object_is_type(state, json_type_string) ? json_object_get_string(state) : "";
            if (!strcmp(s, "ON")) c.led_enabled = 1;
            else if (!strcmp(s, "OFF")) c.led_enabled = 0;
            else valid = false;
            found = true;
        }
    } else if (!strcmp(kind, "child_lock")) {
        valid = valid && get_bool(o, "enabled", &c.child_lock, &found);
    } else valid = false;
    if (valid && found) apply(&c);
    mico_rtos_unlock_mutex(&a1_lock);
    mico_rtos_unlock_mutex(&a1_config_lock);
    if (valid && found && !strcmp(kind, "fan")) feature_manual_fan(c.fan_on);
    return valid && found;
}

void a1_panel_event(bool toggle, int delta)
{
    mico_rtos_lock_mutex(&a1_config_lock);
    mico_rtos_lock_mutex(&a1_lock);
    bool accepted = !a1_config->child_lock && !a1_state.ota_active && !a1_state.reboot_pending;
    if (accepted) {
        a1_config_t c = *a1_config;
        if (toggle) c.fan_on = !c.fan_on;
        if (delta) {
            int speed = c.speed + delta;
            c.speed = speed < 1 ? 1 : speed > 100 ? 100 : speed;
        }
        if (toggle || delta) apply(&c);
    }
    mico_rtos_unlock_mutex(&a1_lock);
    mico_rtos_unlock_mutex(&a1_config_lock);
    if (accepted && (toggle || delta)) { a1_config_t c; a1_config_get(&c); feature_manual_fan(c.fan_on); }
}

void a1_request_ap(void)
{
    mico_rtos_lock_mutex(&a1_lock);
    if (!a1_state.ota_active && !a1_state.reboot_pending) {
        a1_state.ap_requested = true;
        a1_state.led_until = mico_rtos_get_time() + 2000;
    }
    mico_rtos_unlock_mutex(&a1_lock);
}

bool a1_control_json(const char *kind, json_object *o)
{
    if (!a1_control_local(kind, o)) return false;
    feature_group_control(kind, o);
    return true;
}

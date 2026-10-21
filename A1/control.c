#include "a1.h"
#include <ctype.h>
#include "../common/automation.h"

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

static bool control_busy(void)
{
    mico_rtos_lock_mutex(&a1_lock);
    bool busy=a1_state.ota_active || a1_state.reboot_pending || a1_state.wifi_trial_active;
    mico_rtos_unlock_mutex(&a1_lock);
    return busy;
}

/* Serialize only at the group transport boundary, after the local mutation.
 * With no group, local controls allocate no JSON at all. */
static void control_broadcast(const a1_control_t *r)
{
    if (!feature_group_active()) return;
    uint64_t failures=service_memory_failures();
    json_object *o=json_object_new_object();
    if (!o) return;
    if (r->fields&A1_SET_ON) json_build_object_add(o,"on",json_object_new_boolean(r->on));
    if (r->fields&A1_SET_SPEED) json_build_object_add(o,"speed",json_object_new_int(r->speed));
    if (r->fields&A1_SET_MODE) json_build_object_add(o,"mode",json_object_new_string(r->mode==2?"auto":"manual"));
    if (r->fields&A1_SET_ENABLED) json_build_object_add(o,"enabled",json_object_new_boolean(r->enabled));
    if (r->fields&A1_SET_ALWAYS) json_build_object_add(o,"always_on",json_object_new_boolean(r->always_on));
    if (r->fields&A1_SET_BRIGHTNESS) json_build_object_add(o,"brightness",json_object_new_int(r->brightness));
    if (r->fields&A1_SET_COLOR) {
        json_object *color=json_object_new_object();
        json_build_object_add(color,"r",json_object_new_int(r->red));
        json_build_object_add(color,"g",json_object_new_int(r->green));
        json_build_object_add(color,"b",json_object_new_int(r->blue));
        json_build_object_add(o,"color",color);
    }
    if(!service_caller_graph_failed(failures))
        feature_group_control(r->kind==A1_CONTROL_FAN?"fan":r->kind==A1_CONTROL_LIGHT?"light":"child_lock",o);
    json_object_put(o);
}

bool a1_control_apply(a1_control_t *r, bool button, bool broadcast)
{
    if (!r || !r->fields || r->kind>A1_CONTROL_LOCK || control_busy()) return false;
    a1_config_t before; a1_config_get(&before);
    if (button && before.child_lock && r->kind!=A1_CONTROL_LOCK) return false;
    int fan_mode=1;
    if (r->kind==A1_CONTROL_FAN) {
        if (r->speed>100 || r->mode>3 || r->on>2) return false;
        fan_mode=feature_fan_apply(r->fields&A1_SET_MODE?r->mode:0,
                                  r->fields&A1_SET_SPEED?r->speed:-1);
        if (!fan_mode) return false;
        if (r->fields&A1_SET_MODE) r->mode=fan_mode;
    }
    mico_rtos_lock_mutex(&a1_config_lock);
    mico_rtos_lock_mutex(&a1_lock);
    a1_config_t c=*a1_config;
    bool accepted=!a1_state.ota_active && !a1_state.reboot_pending && !a1_state.wifi_trial_active &&
        (!button || !c.child_lock || r->kind==A1_CONTROL_LOCK);
    if (accepted) {
        if (r->kind==A1_CONTROL_FAN) {
            bool was_on=c.fan_on;
            if (r->fields&A1_SET_SPEED) {
                c.fan_on=r->speed!=0;
                if (r->speed && fan_mode==1) c.speed=r->speed;
            }
            if (r->fields&A1_SET_ON) c.fan_on=r->on==2?!was_on:r->on;
            r->on=c.fan_on;
            if (!c.fan_on) a1_state.panel_feedback_until=0;
            if (r->fields&A1_SET_MODE) ++a1_state.mode_feedback_revision;
        } else if (r->kind==A1_CONTROL_LIGHT) {
            if (r->fields&A1_SET_ENABLED) c.led_enabled=r->enabled==2?!c.led_enabled:r->enabled;
            else if (r->fields&(A1_SET_COLOR|A1_SET_BRIGHTNESS)) c.led_enabled=1;
            if (r->fields&A1_SET_ALWAYS) c.led_always_on=r->always_on==2?!c.led_always_on:r->always_on;
            if (r->fields&A1_SET_BRIGHTNESS) c.brightness=r->brightness;
            if (r->fields&A1_SET_COLOR) {c.red=r->red;c.green=r->green;c.blue=r->blue;}
            r->enabled=c.led_enabled;r->always_on=c.led_always_on;
        } else {
            c.child_lock=r->enabled==2?!c.child_lock:r->enabled;
            r->enabled=c.child_lock;
        }
        apply(&c);
    }
    mico_rtos_unlock_mutex(&a1_lock);
    mico_rtos_unlock_mutex(&a1_config_lock);
    if (accepted && r->kind==A1_CONTROL_FAN) feature_manual_fan(c.fan_on);
    if (accepted && broadcast) control_broadcast(r);
    return accepted;
}

/* JSON adapters validate completely before entering the typed business path. */
static bool control_parse(const char *kind, json_object *o, a1_control_t *r)
{
    if (!o || !json_object_is_type(o,json_type_object)) return false;
    memset(r,0,sizeof(*r));
    bool found=false;
#define BOOL_FIELD(key,member,flag) do {found=false;if(!get_bool(o,key,&r->member,&found))return false;if(found)r->fields|=flag;}while(0)
#define BYTE_FIELD(key,member,flag,max) do {found=false;if(!get_byte(o,key,&r->member,max,&found))return false;if(found)r->fields|=flag;}while(0)
    if (!strcmp(kind,"fan")) {
        r->kind=A1_CONTROL_FAN;
        BOOL_FIELD("on",on,A1_SET_ON);BYTE_FIELD("speed",speed,A1_SET_SPEED,100);
        json_object *m=json_object_object_get(o,"mode");
        if(m){
            if(!json_object_is_type(m,json_type_string) || strlen(json_object_get_string(m))!=(size_t)json_object_get_string_len(m))return false;
            const char *v=json_object_get_string(m);
            if(!strcmp(v,"auto"))r->mode=2;else if(!strcmp(v,"manual"))r->mode=1;else return false;
            r->fields|=A1_SET_MODE;
        }
    } else if (!strcmp(kind,"light")) {
        r->kind=A1_CONTROL_LIGHT;
        BOOL_FIELD("enabled",enabled,A1_SET_ENABLED);BOOL_FIELD("always_on",always_on,A1_SET_ALWAYS);
        BYTE_FIELD("brightness",brightness,A1_SET_BRIGHTNESS,255);
        json_object *color=json_object_object_get(o,"color");
        if(color){
            bool red=false,green=false,blue=false;
            if(!json_object_is_type(color,json_type_object) ||
               !get_byte(color,"r",&r->red,255,&red) || !get_byte(color,"g",&r->green,255,&green) ||
               !get_byte(color,"b",&r->blue,255,&blue) || !red || !green || !blue)return false;
            r->fields|=A1_SET_COLOR;
        }
        json_object *state=json_object_object_get(o,"state");
        if(state){
            if(!json_object_is_type(state,json_type_string))return false;
            const char *s=json_object_get_string(state);
            if(!strcmp(s,"ON"))r->enabled=1;else if(!strcmp(s,"OFF"))r->enabled=0;else return false;
            r->fields|=A1_SET_ENABLED;
        }
    } else if (!strcmp(kind,"child_lock")) {
        r->kind=A1_CONTROL_LOCK;BOOL_FIELD("enabled",enabled,A1_SET_ENABLED);
    } else return false;
#undef BOOL_FIELD
#undef BYTE_FIELD
    return r->fields!=0;
}
bool a1_control_local(const char *kind, json_object *o)
{
    a1_control_t r;return control_parse(kind,o,&r) && a1_control_apply(&r,false,false);
}
bool a1_control_json(const char *kind, json_object *o)
{
    a1_control_t r;return control_parse(kind,o,&r) && a1_control_apply(&r,false,true);
}

bool a1_panel_inspect(void)
{
    a1_config_t c;a1_config_get(&c);
    if(c.child_lock || control_busy())return false;
    if(!c.fan_on){
        a1_control_t r={.kind=A1_CONTROL_FAN,.fields=A1_SET_ON,.on=1};
        if(!a1_control_apply(&r,true,true))return false;
    }
    mico_rtos_lock_mutex(&a1_lock);
    bool accepted=!a1_state.ota_active && !a1_state.reboot_pending && !a1_state.wifi_trial_active;
    if(accepted){++a1_state.mode_feedback_revision;a1_state.panel_feedback_until=mico_rtos_get_time()+4200;a1_state.fan_feedback_until=0;}
    mico_rtos_unlock_mutex(&a1_lock);return accepted;
}
void a1_panel_event(bool toggle, int delta)
{
    if((!toggle && !delta) || !service_control_begin())return;
    a1_config_t c;a1_config_get(&c);
    a1_control_t r={.kind=A1_CONTROL_FAN};
    if(delta){
        bool automatic=feature_fan_auto();
        int speed=(automatic?feature_fan_limit():(c.fan_on?c.speed:0))+delta;
        if(speed<(automatic?1:0))speed=automatic?1:0;if(speed>100)speed=100;
        r.fields|=A1_SET_SPEED;r.speed=speed;
    }
    if(toggle){r.fields|=A1_SET_ON;r.on=2;}
    if(a1_control_apply(&r,true,true) && delta){
        mico_rtos_lock_mutex(&a1_lock);a1_state.fan_feedback_until=mico_rtos_get_time()+3000;mico_rtos_unlock_mutex(&a1_lock);
    }
    service_control_end();
}

bool a1_automation_execute(const automation_action_t *a, bool button)
{
    if(!a || a->kind>AUTO_ACTION_MAX)return false;
    if(a->kind==AUTO_NONE)return true;
    if(!service_control_begin())return false;
    a1_config_t c;a1_config_get(&c);
    bool ok=false;
    if(button && c.child_lock && a->kind!=AUTO_CHILD_LOCK){service_event("info","button","child_locked");goto done;}
    if(control_busy())goto done;
    a1_control_t r={0};
    switch(a->kind){
    case AUTO_FAN:
        r.kind=A1_CONTROL_FAN;r.fields=A1_SET_ON;r.on=a->value;
        if(a->speed){r.fields|=A1_SET_SPEED;r.speed=a->speed;}
        break;
    case AUTO_LIGHT:
        r.kind=A1_CONTROL_LIGHT;r.fields=A1_SET_ENABLED;r.enabled=a->value;
        if(a->speed<=100){r.fields|=A1_SET_BRIGHTNESS;r.brightness=(a->speed*255+50)/100;}
        break;
    case AUTO_CHILD_LOCK:r.kind=A1_CONTROL_LOCK;r.fields=A1_SET_ENABLED;r.enabled=a->value;break;
    case AUTO_LIGHT_ALWAYS:r.kind=A1_CONTROL_LIGHT;r.fields=A1_SET_ALWAYS;r.always_on=a->value;break;
    case AUTO_FAN_MODE:case AUTO_FAN_MODE_TOGGLE:
        r.kind=A1_CONTROL_FAN;r.fields=A1_SET_MODE;r.mode=a->kind==AUTO_FAN_MODE_TOGGLE?3:a->value?2:1;break;
    case AUTO_FAN_PANEL:ok=a1_panel_inspect();goto done;
    case AUTO_LIGHT_EFFECT:ok=feature_light_select(a->value);goto done;
    case AUTO_FILTER_RESET:ok=!strcmp(a1_filter_change(0,true),"accepted");goto done;
    case AUTO_REBOOT:ok=a1_reboot_later(1500);goto done;
    default:goto done;
    }
    ok=a1_control_apply(&r,button,true);
done:
    service_control_end();
    service_event(ok?"info":"warn","command",ok?"accepted":"rejected");
    return ok;
}

void a1_request_ap(void)
{
    mico_rtos_lock_mutex(&a1_lock);
    if(!a1_state.ota_active && !a1_state.reboot_pending){a1_state.ap_requested=true;a1_state.led_until=mico_rtos_get_time()+2000;}
    mico_rtos_unlock_mutex(&a1_lock);
}

/* Hardware producer, business-worker consumer. Both hold a1_lock only for
 * bounded copies; no allocation, Flash, JSON, or network work on enqueue. */
#define A1_INPUT_CAPACITY 16u
#define A1_INPUT_MAX_AGE_MS 2000u
typedef struct {uint32_t at;uint8_t kind,key,clicks,seconds;int16_t delta;} a1_input_t;
static a1_input_t inputs[A1_INPUT_CAPACITY];
static unsigned input_head,input_count,input_dropped;
void a1_input_clear(void)
{
    mico_rtos_lock_mutex(&a1_lock);input_head=input_count=0;mico_rtos_unlock_mutex(&a1_lock);
}
bool a1_input_push(unsigned kind,unsigned key,unsigned clicks,unsigned seconds,int delta)
{
    if(kind>A1_INPUT_FULL_RESET || (kind==A1_INPUT_BUTTON &&
       (key>=AUTOMATION_KEYS || clicks>30 || seconds>4 || (!!clicks==!!seconds))))return false;
    mico_rtos_lock_mutex(&a1_lock);
    bool ok=!a1_state.ota_active && !a1_state.reboot_pending && !a1_state.wifi_trial_active;
    if(ok && kind==A1_INPUT_SLIDER && input_count){
        a1_input_t *last=&inputs[(input_head+input_count-1)%A1_INPUT_CAPACITY];
        /* Same-direction slider runs can merge without changing clamp order. */
        if(last->kind==kind && ((last->delta>0 && delta>0) || (last->delta<0 && delta<0))){
            int sum=last->delta+delta;last->delta=sum>100?100:sum< -100?-100:sum;
            last->at=mico_rtos_get_time();mico_rtos_unlock_mutex(&a1_lock);return true;
        }
    }
    if(ok && input_count<A1_INPUT_CAPACITY){
        inputs[(input_head+input_count++)%A1_INPUT_CAPACITY]=(a1_input_t){mico_rtos_get_time(),kind,key,clicks,seconds,delta};
    }else{if(ok)++input_dropped;ok=false;}
    mico_rtos_unlock_mutex(&a1_lock);return ok;
}
void a1_input_poll(void)
{
    /* Bound work per pass so binding/control deadlines keep advancing. */
    for(unsigned n=0;n<8;n++){
        mico_rtos_lock_mutex(&a1_lock);
        bool busy=a1_state.ota_active || a1_state.reboot_pending || a1_state.wifi_trial_active;
        if(busy){input_head=input_count=0;}
        bool dropped=input_dropped!=0;input_dropped=0;
        bool present=input_count!=0;a1_input_t e={0};
        if(present){e=inputs[input_head];input_head=(input_head+1)%A1_INPUT_CAPACITY;--input_count;}
        mico_rtos_unlock_mutex(&a1_lock);
        if(dropped)service_event("warn","button","busy");
        if(!present)return;
        if((uint32_t)(mico_rtos_get_time()-e.at)>A1_INPUT_MAX_AGE_MS){service_event("warn","button","missed");continue;}
        if(e.kind==A1_INPUT_BUTTON)automation_button(e.key,e.clicks,e.seconds);
        else if(e.kind==A1_INPUT_SLIDER)a1_panel_event(false,e.delta);
        else if(service_control_begin()){
            if(!control_busy())a1_reset(e.kind==A1_INPUT_FULL_RESET);
            service_control_end();
        }
    }
}

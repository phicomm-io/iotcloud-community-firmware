/* Production local-control and input handoff, with JSON admission unavailable.
 * Feature persistence is tested separately by the production runtime simulator. */
#define FEATURE_DEVICE_A1
#include "../A1/a1.h"
#include "../common/automation.h"
#include <assert.h>

uint32_t test_now;
uint32_t mico_rtos_get_time(void){return test_now;}
mico_mutex_t a1_lock,a1_config_lock;
a1_state_t a1_state;
static a1_config_t config={.speed=60,.led_enabled=1,.brightness=255};
a1_config_t *a1_config=&config;
static automation_config_t automations;
static unsigned changes,json_attempts,actions,resets,reset_full,missed,busy_logs;
static bool service_locked,automatic;
static unsigned limit=100,effect;
void a1_config_get(a1_config_t *out){*out=config;}
void a1_control_changed(void){++changes;}
bool service_control_begin(void){assert(!service_locked);service_locked=true;return true;}
void service_control_end(void){assert(service_locked);service_locked=false;}
uint64_t service_memory_failures(void){return 0;}
bool service_caller_graph_failed(uint64_t before){(void)before;return false;}
void service_event(const char *l,const char *m,const char *e){(void)l;(void)m;if(!strcmp(e,"missed"))++missed;if(!strcmp(e,"busy"))++busy_logs;}
bool feature_group_active(void){return false;}
bool feature_group_control(const char *kind,json_object *o){(void)kind;(void)o;abort();}
bool feature_fan_auto(void){return automatic;}
unsigned feature_fan_limit(void){return limit;}
int feature_fan_apply(unsigned mode,int speed){if(mode)automatic=mode==3?!automatic:mode==2;if(automatic && speed>0)limit=speed;return automatic?2:1;}
void feature_manual_fan(bool on){(void)on;}
bool feature_light_select(unsigned v){effect=v==255?(effect+1)%5:v;return true;}
const char *a1_filter_change(unsigned h,bool reset){(void)h;assert(reset);++actions;return "accepted";}
bool a1_reboot_later(uint32_t n){(void)n;++actions;return true;}
bool a1_reset(bool full){++resets;reset_full+=full;return true;}
void automation_load(automation_config_t *out){*out=automations;}
bool automation_save(const automation_config_t *c){automations=*c;return true;}
static json_object *denied_object(void){++json_attempts;return NULL;}
#define json_object_new_object denied_object
#include "../A1/control.c"
#include "../common/automation.c"
#undef json_object_new_object

static void press(unsigned clicks){assert(a1_input_push(A1_INPUT_BUTTON,0,clicks,0,0));a1_input_poll();}
int main(void)
{
    automation_defaults(&automations);
    assert(a1_input_push(A1_INPUT_BUTTON,0,1,0,0));
    assert(!config.fan_on && !changes); /* enqueue never executes a mutation */
    a1_input_poll();assert(config.fan_on && changes==1);
    unsigned changed=changes;press(1);assert(changes==changed && a1_state.panel_feedback_until);
    press(2);assert(automatic && config.fan_on);press(2);assert(!automatic && config.fan_on);
    assert(a1_input_push(A1_INPUT_BUTTON,0,0,1,0));a1_input_poll();assert(!config.fan_on);
    config.child_lock=1;press(1);assert(!config.fan_on);
    automations.buttons[0][2]=(automation_action_t){.kind=AUTO_CHILD_LOCK,.value=2};
    press(3);assert(!config.child_lock);
    automations.buttons[0][3]=(automation_action_t){.kind=AUTO_FAN,.value=2,.speed=42};
    press(4);assert(config.fan_on && config.speed==42);press(4);assert(!config.fan_on);
    automations.buttons[0][4]=(automation_action_t){.kind=AUTO_LIGHT,.value=2,.speed=50};
    press(5);assert(!config.led_enabled && config.brightness==128);
    automations.buttons[0][5]=(automation_action_t){.kind=AUTO_LIGHT_ALWAYS,.value=2};
    press(6);assert(config.led_always_on);
    automations.buttons[0][6]=(automation_action_t){.kind=AUTO_LIGHT_EFFECT,.value=255};
    for(unsigned i=0;i<6;i++)press(7);assert(effect==1);
    automations.buttons[0][7]=(automation_action_t){.kind=AUTO_FILTER_RESET};press(8);assert(actions==1);
    automations.buttons[0][8]=(automation_action_t){.kind=AUTO_REBOOT};press(9);assert(actions==2);

    config.fan_on=1;config.speed=50;
    assert(a1_input_push(A1_INPUT_SLIDER,0,0,0,10));
    assert(a1_input_push(A1_INPUT_SLIDER,0,0,0,15));
    a1_input_poll();assert(config.speed==75);
    /* Preserve direction changes at the clamp: 75+50-10 = 90, not 100. */
    assert(a1_input_push(A1_INPUT_SLIDER,0,0,0,50));
    assert(a1_input_push(A1_INPUT_SLIDER,0,0,0,-10));a1_input_poll();assert(config.speed==90);
    changed=changes;assert(a1_input_push(A1_INPUT_SLIDER,0,0,0,-10));test_now+=2001;
    a1_input_poll();assert(changes==changed && missed==1);
    test_now=UINT32_MAX-100;assert(a1_input_push(A1_INPUT_SLIDER,0,0,0,-10));test_now+=200;
    a1_input_poll();assert(config.speed==80); /* uptime wrap */

    for(unsigned i=0;i<16;i++)assert(a1_input_push(A1_INPUT_BUTTON,0,2,0,0));
    assert(!a1_input_push(A1_INPUT_BUTTON,0,1,0,0));
    a1_input_poll();assert(input_count==8 && busy_logs==1);
    a1_input_poll();assert(input_count==0 && !automatic);
    assert(a1_input_push(A1_INPUT_BUTTON,0,2,0,0));a1_state.ota_active=true;
    assert(!a1_input_push(A1_INPUT_BUTTON,0,1,0,0));a1_input_poll();a1_state.ota_active=false;
    a1_input_poll();assert(!automatic && input_count==0);
    assert(a1_input_push(A1_INPUT_BUTTON,0,2,0,0));a1_input_clear();a1_input_poll();assert(!automatic);
    a1_state.wifi_trial_active=true;
    assert(!a1_input_push(A1_INPUT_BUTTON,0,2,0,0));a1_state.wifi_trial_active=false;
    assert(a1_input_push(A1_INPUT_NETWORK_RESET,0,0,0,0));a1_input_poll();assert(resets==1 && !reset_full);
    assert(a1_input_push(A1_INPUT_FULL_RESET,0,0,0,0));a1_input_poll();assert(resets==2 && reset_full==1);
    assert(!a1_input_push(A1_INPUT_BUTTON,3,1,0,0));
    assert(!a1_input_push(A1_INPUT_BUTTON,0,1,1,0));
    assert(json_attempts==0 && !service_locked);
    puts("PASS A1 typed controls without JSON: gestures, toggles, lock, bounded FIFO, overflow, expiry, busy, reset, wrap");
    return 0;
}

/* Independently frozen TC1 v12 from base6950338. */
typedef struct system_config_t system_config_t;
#include "TC1/main.h"
#include "TC1/config_legacy.h"
#include <assert.h>
typedef struct
{
    char version;
    char mqtt_ip[SETTING_MQTT_STRING_LENGTH_MAX];
    char socket_names[SOCKET_NUM][SOCKET_NAME_LENGTH];
    int mqtt_port;
    int mqtt_report_freq;
    char legacy_mqtt_user[SETTING_MQTT_STRING_LENGTH_MAX];
    char legacy_mqtt_password[SETTING_MQTT_STRING_LENGTH_MAX];
    char socket_status[SOCKET_NUM]; //记录当前开关
    char user[maxNameLen];
    WiFiEvent last_wifi_status;
    char ap_name[32];
    char ap_key[32];
    int task_count;
    int p_count_2_days_ago;
    int p_count_1_day_ago;
    int power_led_enabled;
    pTimedTask task_top;
    struct TimedTask timed_tasks[MAX_TASK_NUM];
    char web_password[WEB_PASSWORD_LENGTH_MAX + 1];
    uint16_t power_coefficient_x100;
    char mqtt_user[MQTT_USER_MAX + 1];
    char mqtt_password[MQTT_PASSWORD_MAX + 1];
    uint8_t mqtt_topic_mode;
    char mqtt_host[128];
    automation_config_t automation;
    bemfa_config_t bemfa;
} baseline_tc1_v12_t;
int main(void){
 assert(sizeof(baseline_tc1_v12_t)==sizeof(user_config_v12_t));
 assert(offsetof(user_config_t,local_auth)==sizeof(baseline_tc1_v12_t));
 #define CHECK(k) assert(offsetof(user_config_t,k)==offsetof(baseline_tc1_v12_t,k))
 CHECK(version);CHECK(mqtt_ip);CHECK(socket_names);CHECK(task_top);CHECK(timed_tasks);
 CHECK(web_password);CHECK(mqtt_host);CHECK(automation);CHECK(bemfa);
 user_config_t current;memset(&current,0xa5,sizeof(current));
 baseline_tc1_v12_t previous;memcpy(&previous,&current,sizeof(previous));
 memset(&current.local_auth,0x3c,sizeof(current.local_auth));assert(!memcmp(&current,&previous,sizeof(previous)));
 puts("TC1 frozen v12 prefix preserved");return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

#include "main.h"
#include "user_gpio.h"
#include "mqtt_server/user_mqtt_client.h"
#include "timed_task/timed_task.h"
#include "http_server/web_log.h"
#include "user_wifi.h"
#include "http_server/app_httpd.h"

static const int day_sec = 86400;
static mico_mutex_t task_lock;
static void rebuild_tasks(void) {
    user_config->task_top = NULL;
    user_config->task_count = 0;
    for (int i = 0; i < MAX_TASK_NUM; ++i) {
        pTimedTask t = &user_config->timed_tasks[i];
        t->next = NULL;
        if (!t->on_use)
            continue;
        if (t->prs_time <= 0 || t->operation < 0 || t->operation > 13 || t->on < 0 || t->on > 1 ||
            t->weekday < 0 || t->weekday > 8) {
            t->on_use = false;
            continue;
        }
        pTimedTask *link = &user_config->task_top;
        while (*link && (*link)->prs_time <= t->prs_time)
            link = &(*link)->next;
        t->next = *link;
        *link = t;
        ++user_config->task_count;
    }
}
void TasksInit(void) {
    mico_rtos_init_mutex(&task_lock);
    rebuild_tasks();
}
bool AddTaskValue(time_t timestamp, int operation, int on, int weekday) {
    if (timestamp <= 0 || operation < 0 || operation > 13 || on < 0 || on > 1 || weekday < 0 ||
        weekday > 8)
        return false;
    mico_rtos_lock_mutex(&task_lock);
    pTimedTask t = NULL;
    for (int i = 0; i < MAX_TASK_NUM; ++i)
        if (!user_config->timed_tasks[i].on_use) {
            t = &user_config->timed_tasks[i];
            break;
        }
    if (t) {
        memset(t, 0, sizeof(*t));
        t->on_use = true;
        t->prs_time = timestamp;
        t->operation = operation;
        t->on = on;
        t->weekday = weekday;
        if (weekday > 0 && weekday < 8) {
            time_t local = time(NULL) + 28800;
            int today = (local / day_sec + 3) % 7 + 1;
            int days = (weekday - today + 7) % 7;
            int second = (timestamp + 28800) % day_sec;
            if (!days && second <= local % day_sec)
                days = 7;
            t->prs_time = local - local % day_sec + days * day_sec + second - 28800;
        }
        rebuild_tasks();
    }
    mico_rtos_unlock_mutex(&task_lock);
    if (t)
        mico_system_context_update(sys_config);
    return t != NULL;
}
bool DelTask(int timestamp) {
    bool found = false;
    mico_rtos_lock_mutex(&task_lock);
    for (int i = 0; i < MAX_TASK_NUM; ++i)
        if (user_config->timed_tasks[i].on_use &&
            user_config->timed_tasks[i].prs_time == timestamp) {
            user_config->timed_tasks[i].on_use = false;
            found = true;
            break;
        }
    rebuild_tasks();
    mico_rtos_unlock_mutex(&task_lock);
    if (found)
        mico_system_context_update(sys_config);
    return found;
}
int TaskSnapshot(struct TimedTask *out, int capacity) {
    int count = 0;
    mico_rtos_lock_mutex(&task_lock);
    for (pTimedTask t = user_config->task_top; t && count < capacity; t = t->next) {
        out[count] = *t;
        out[count++].next = NULL;
    }
    mico_rtos_unlock_mutex(&task_lock);
    return count;
}
bool TasksReplace(const struct TimedTask *tasks, int count) {
    if (count < 0 || count > MAX_TASK_NUM || (count && !tasks))
        return false;
    mico_rtos_lock_mutex(&task_lock);
    memset(user_config->timed_tasks, 0, sizeof(user_config->timed_tasks));
    if (count)
        memcpy(user_config->timed_tasks, tasks, count * sizeof(*tasks));
    rebuild_tasks();
    mico_rtos_unlock_mutex(&task_lock);
    return mico_system_context_update(sys_config) == kNoErr;
}
void ProcessTask() {
    struct TimedTask task;
    mico_rtos_lock_mutex(&task_lock);
    pTimedTask first = user_config->task_top;
    if (!first || time(NULL) < first->prs_time) {
        mico_rtos_unlock_mutex(&task_lock);
        return;
    }
    task = *first;
    first->on_use = false;
    if (task.weekday) {
        first->on_use = true;
        time_t period = (task.weekday == 8 ? 1 : 7) * day_sec;
        time_t now = time(NULL);
        first->prs_time += ((now - first->prs_time) / period + 1) * period;
    }
    rebuild_tasks();
    mico_rtos_unlock_mutex(&task_lock);
    mico_system_context_update(sys_config);
    task_log("process task time[%ld] operation[%s] on[%d]", task.prs_time,
             get_func_name(task.operation), task.on);
    switch (task.operation) {
    case SWITCH_ALL_SOCKETS:
        UserRelaySetAll(task.on);
        mico_system_context_update(sys_config);
        for (int i = 0; i < SOCKET_NUM; i++) {
            UserMqttSendSocketState(i);
        }
        UserMqttSendTotalSocketState();
        break;
    case SWITCH_SOCKET_1:
    case SWITCH_SOCKET_2:
    case SWITCH_SOCKET_3:
    case SWITCH_SOCKET_4:
    case SWITCH_SOCKET_5:
    case SWITCH_SOCKET_6:
        UserRelaySet(task.operation - 1, task.on);
        UserMqttSendSocketState(task.operation - 1);
        UserMqttSendTotalSocketState();
        mico_system_context_update(sys_config);
        break;
    case SWITCH_LED_ENABLE:
        user_config->power_led_enabled = task.on;
        if (RelayOut() && task.on) {
            UserLedSet(1);
        } else {
            UserLedSet(0);
        }
        UserMqttSendLedState();
        mico_system_context_update(sys_config);
        break;
    case SWITCH_CHILD_LOCK_ENABLE:
        user_config->user[0] = task.on;
        childLockEnabled = user_config->user[0];
        mico_system_context_update(sys_config);
        UserMqttSendChildLockState();
        break;
    case REBOOT_SYSTEM:
        MicoSystemReboot();
        break;
    case REBOOT_HTTP:
        AppHttpdStop();
        mico_rtos_thread_sleep(1);
        AppHttpdStart();
        break;
    case REBOOT_SOFT:
        UserSoftReboot();
        break;
    case CONFIG_WIFI:

        micoWlanSuspendStation();
        ApInit(true);
        break;
    case RESET_SYSTEM:

        mico_system_context_restore(sys_config);
        mico_rtos_thread_sleep(1);
        MicoSystemReboot();
        break;
    default:
        break;
    }
}

char *GetTaskStr(void) {
    struct TimedTask *tasks = malloc(MAX_TASK_NUM * sizeof(*tasks));
    if (!tasks)
        return NULL;
    int count = TaskSnapshot(tasks, MAX_TASK_NUM);
    size_t capacity = (size_t)count * 100 + 3, used = 1;
    char *text = malloc(capacity);
    if (!text) {
        free(tasks);
        return NULL;
    }
    text[0] = '[';
    for (int i = 0; i < count; ++i) {
        int n = snprintf(text + used, capacity - used,
                         "%s{\"timestamp\":%ld,\"operation\":%d,\"on\":%d,\"weekday\":%d}",
                         i ? "," : "", (long)tasks[i].prs_time, tasks[i].operation, tasks[i].on,
                         tasks[i].weekday);
        if (n < 0 || (size_t)n >= capacity - used) {
            free(text);
            free(tasks);
            return NULL;
        }
        used += n;
    }
    snprintf(text + used, capacity - used, "]");
    free(tasks);
    return text;
}

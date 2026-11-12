/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include <stdbool.h>
/* Shared TC1/S30 wall-clock rules. Weekdays start with Monday, UTC epochs are
 * persisted; neither reboot nor a backwards correction replays an occurrence. */
typedef struct { uint8_t kind,target,value,speed,red,green,blue,reserved; } ph_schedule_action;
typedef struct {
    uint16_t id,minute;
    uint8_t enabled,days,once,state;
    uint32_t at,last;
    ph_schedule_action action;
} ph_schedule_task;
typedef char ph_schedule_action_abi[(sizeof(ph_schedule_action)==8)?1:-1];
typedef char ph_schedule_task_abi[(sizeof(ph_schedule_task)==24)?1:-1];
static inline uint32_t ph_schedule_at(const ph_schedule_task *t,uint32_t now,int zone) {
    if(t->once)return t->at;
    int64_t local=(int64_t)now+zone*60,day=local/86400;
    unsigned weekday=(unsigned)((day+3)%7);
    if(!(t->days&(1u<<weekday)))return 0;
    return (uint32_t)(day*86400+t->minute*60-zone*60);
}
static inline bool ph_schedule_due(const ph_schedule_task *t,uint32_t at,uint32_t now,uint32_t previous,uint32_t highwater) {
    return t->id&&t->enabled&&t->once<2&&at&&at<=now&&at>t->last&&
        (t->once||(at>previous&&at>highwater));
}
static inline bool ph_schedule_timely(uint32_t at,uint32_t now,uint32_t highwater) {
    return now-at<=60&&at>highwater;
}
static inline bool ph_schedule_jump(uint32_t now,uint32_t previous,uint32_t ms,uint32_t previous_ms) {
    int64_t delta=(int64_t)now-previous-(uint32_t)(ms-previous_ms)/1000;
    return delta>2||delta< -2;
}

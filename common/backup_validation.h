/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "automation.h"
static bool ph_backup_action_valid(const automation_action_t *a){
    return a->kind<=AUTO_ACTION_MAX && a->speed<=100 &&
        (a->kind!=AUTO_OUTLET || a->target<6 || a->target==255) &&
        (a->kind!=AUTO_FAN || a->value<=2) &&
        (a->kind!=AUTO_LIGHT_EFFECT || a->value<=5);
}
static bool ph_backup_automation_valid(const automation_storage_t *c){
    if(c->magic!=AUTOMATION_MAGIC || c->timezone < -720 || c->timezone>840)return false;
    for(unsigned i=0;i<AUTOMATION_TASKS;i++){
        const automation_task_t *t=&c->tasks[i];
        if(t->minute>=1440 || t->enabled>1 || t->days>127 || t->once>2 || t->state>AUTO_CANCELLED || !ph_backup_action_valid(&t->action))return false;
    }
    for(unsigned k=0;k<2;k++)for(unsigned i=0;i<34;i++)if(!ph_backup_action_valid(&c->buttons[k][i]))return false;
    return true;
}

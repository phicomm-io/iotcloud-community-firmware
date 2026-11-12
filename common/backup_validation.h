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
        if(t->minute>=1440 || t->enabled>1 || t->days>127 || t->state>AUTO_CANCELLED || !ph_backup_action_valid(&t->action))return false;
#ifdef AUTOMATION_TC1
        if(t->once==3){ph_power_rule r;if(!tc1_power_rule_from_task(t,&r))return false;}
        else if(t->once>2)return false;
#else
        if(t->once>2)return false;
#endif
    }
#ifdef AUTOMATION_TC1
    unsigned power_count=0;
    for(unsigned i=0;i<AUTOMATION_TASKS;i++)if(c->tasks[i].id&&c->tasks[i].once==3){
        if(++power_count>PH_POWER_RULES_MAX)return false;
        for(unsigned j=0;j<i;j++)if(c->tasks[j].id==c->tasks[i].id)return false;
    }
#endif
    for(unsigned k=0;k<2;k++)for(unsigned i=0;i<34;i++)if(!ph_backup_action_valid(&c->buttons[k][i]))return false;
    return true;
}

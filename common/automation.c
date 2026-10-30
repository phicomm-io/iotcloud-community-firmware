#include "automation.h"
#include "service_mp.h"
#include "mico.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>
static mico_mutex_t automation_lock;
static bool automation_ready;
static uint32_t previous, highwater, previous_ms;
static bool armed;
static void auto_init(void);
#ifdef AUTOMATION_TC1
typedef struct {ph_power_rule rule;ph_power_trigger trigger;} auto_power_state;
static auto_power_state power_states[PH_POWER_RULES_MAX];
static void auto_power_reset(unsigned id){
 for(unsigned i=0;i<PH_POWER_RULES_MAX;i++)if(power_states[i].rule.id==id)memset(&power_states[i],0,sizeof(power_states[i]));
}
void automation_power_manual_override(void){
 auto_init();if(!automation_ready)return;
 mico_rtos_lock_mutex(&automation_lock);
 ph_power_rule rules[PH_POWER_RULES_MAX];unsigned count=0;
 /* A key can precede the first worker tick. Own configured rules here too,
  * so that an unseen condition cannot undo the user's choice on that tick. */
 if(tc1_automation_load_power_rules(rules,&count)){
  memset(power_states,0,sizeof(power_states));
  for(unsigned i=0;i<count;i++)power_states[i].rule=rules[i];
 }
 for(unsigned i=0;i<PH_POWER_RULES_MAX;i++)ph_power_trigger_override(&power_states[i].trigger);
 mico_rtos_unlock_mutex(&automation_lock);
}
#endif
#ifdef FEATURE_DEVICE_A1
typedef struct { uint16_t id; uint32_t started; } auto_countdown_t;
static auto_countdown_t countdowns[AUTOMATION_TASKS];
static bool countdown_ready;
/* Called with automation_lock held. Flash records describe the duration, never
 * an uptime deadline that could accidentally restart after a reboot. */
static bool auto_countdown_boot(automation_config_t *c) {
 if(countdown_ready)return true;
 bool changed=false;
 for(unsigned i=0;i<AUTOMATION_TASKS;i++)if(c->tasks[i].once==2 && c->tasks[i].enabled){
  c->tasks[i].enabled=0;c->tasks[i].state=AUTO_CANCELLED;changed=true;
 }
 if(changed){c->revision++;if(!automation_save(c))return false;}
 countdown_ready=true;return true;
}
static unsigned auto_remaining(const automation_task_t *t) {
 if(!t->enabled)return 0;
 for(unsigned i=0;i<AUTOMATION_TASKS;i++)if(countdowns[i].id==t->id){
  uint32_t elapsed=(uint32_t)(mico_rtos_get_time()-countdowns[i].started)/1000;
  return elapsed<t->at?t->at-elapsed:0;
 }
 return 0;
}
#endif

void automation_defaults(automation_config_t *c){memset(c,0,sizeof(*c));c->magic=AUTOMATION_MAGIC;c->timezone=480;c->next_id=1;
#ifdef AUTOMATION_TC1
/* TC1 has no default custom key action. Fixed recovery is separate. */
#elif defined(FEATURE_DEVICE_A1)
c->buttons[0][0].kind=AUTO_FAN_PANEL;
for(unsigned i=30;i<34;i++){c->buttons[0][i].kind=AUTO_FAN;c->buttons[0][i].target=255;}
c->buttons[0][1].kind=AUTO_FAN_MODE_TOGGLE;
#endif
}
static void auto_init(void){if(!automation_ready)automation_ready=mico_rtos_init_mutex(&automation_lock)==kNoErr;}
static ph_value auto_get(ph_value o,const char*k){ph_value v={.kind=PHV_MISSING};phv_get(&o,k,&v);return v;}
static bool auto_has(ph_value v){return phv_exists(&v);}
static bool auto_integer(ph_value o,const char*k,int*v,int lo,int hi){ph_value p=auto_get(o,k);int64_t n;if(!phv_int(&p,&n)||n<lo||n>hi)return false;*v=(int)n;return true;}
static bool auto_flag(ph_value o,const char*k,uint8_t*v){ph_value p=auto_get(o,k);bool n;if(!phv_bool(&p,&n))return false;*v=n;return true;}
static bool auto_only(ph_value o,const char*allowed){return phv_keys(&o,allowed);}
static const char *auto_names[]={"none","outlet.set","fan.set","light.set","child_lock.set","indicator.set","display.set","system.reboot","light_effect.select","filter.reset","light_always.set","fan.mode.set","fan.mode.toggle","fan.panel","wake_alarm.start"};
static bool auto_supported(unsigned k){
#ifdef AUTOMATION_TC1
 return k==AUTO_NONE || k==AUTO_OUTLET || k==AUTO_CHILD_LOCK || k==AUTO_INDICATOR || k==AUTO_REBOOT;
#elif defined(FEATURE_DEVICE_A1)
 return k==AUTO_NONE || k==AUTO_FAN || k==AUTO_LIGHT || k==AUTO_CHILD_LOCK || k==AUTO_REBOOT || k==AUTO_LIGHT_EFFECT || k==AUTO_FILTER_RESET || k==AUTO_LIGHT_ALWAYS || k==AUTO_FAN_MODE || k==AUTO_FAN_MODE_TOGGLE || k==AUTO_FAN_PANEL;
#else
 return k==AUTO_NONE || k==AUTO_DISPLAY || k==AUTO_REBOOT || k==AUTO_WAKE_ALARM;
#endif
}
static bool auto_action_parse(ph_value o,automation_action_t *a,bool button,bool envelope){
 memset(a,0,sizeof(*a));ph_value name=auto_get(o,"action"),p=auto_get(o,"params");
 if((envelope && !auto_only(o,"|action||params|")) || !auto_has(name) || !(name.kind==PHV_TEXT) || !auto_has(p) || !(p.kind==PHV_MAP))return false;
 if(phv_equal_text(&name,"light_effect.next")){if(!button || !auto_supported(AUTO_LIGHT_EFFECT) || !auto_only(p,""))return false;a->kind=AUTO_LIGHT_EFFECT;a->value=255;return true;}
 unsigned k;for(k=0;k<=AUTO_ACTION_MAX;k++)if(phv_equal_text(&name,auto_names[k]))break;
 if(k>AUTO_ACTION_MAX || !auto_supported(k))return false;a->kind=k;int n=0;
 if(k==AUTO_FAN_MODE_TOGGLE || k==AUTO_FAN_PANEL)return button && auto_only(p,"");
 if(k==AUTO_FAN_MODE){ph_value m=auto_get(p,"mode");if(!auto_only(p,"|mode|") || !auto_has(m) || !(m.kind==PHV_TEXT))return false;if(!phv_equal_text(&m,"auto") && !phv_equal_text(&m,"manual"))return false;a->value=phv_equal_text(&m,"auto");return true;}
 if(k==AUTO_NONE || k==AUTO_REBOOT || k==AUTO_FILTER_RESET)return (k!=AUTO_FILTER_RESET || button) && auto_only(p,"");
 if(k==AUTO_LIGHT_EFFECT){if(!button || !auto_only(p,"|effect|") || !auto_integer(p,"effect",&n,0,5))return false;a->value=n;return true;}
 if(k==AUTO_FAN || k==AUTO_OUTLET){
  if(!auto_only(p,k==AUTO_FAN?"|on||speed||toggle|":"|on||index||toggle|"))return false;
  uint8_t value;ph_value toggle=auto_get(p,"toggle");
  if(auto_has(toggle)){if(!button || !auto_flag(p,"toggle",&value) || !value || auto_has(auto_get(p,"on")))return false;a->value=2;}
  else {if(!auto_flag(p,"on",&value))return false;a->value=value;}
  a->target=255;a->speed=0;
  if(auto_has(auto_get(p,"index"))){if(!auto_integer(p,"index",&n,0,5))return false;a->target=n;}
  if(auto_has(auto_get(p,"speed"))){if(!auto_integer(p,"speed",&n,1,100))return false;a->speed=n;}
  return true;
 }
 if(k==AUTO_WAKE_ALARM){
  if(button || !auto_only(p,"|duration_seconds||tone|") || !auto_integer(p,"duration_seconds",&n,1,180))return false;
  a->value=n;
  if(auto_has(auto_get(p,"tone"))){if(!auto_integer(p,"tone",&n,0,5))return false;a->speed=n;}
  return true;
 }
 if(k==AUTO_DISPLAY){if(!auto_only(p,"|brightness|") || !auto_integer(p,"brightness",&n,0,100) || n%25)return false;a->value=n;return true;}
 if(k==AUTO_LIGHT || k==AUTO_CHILD_LOCK || k==AUTO_INDICATOR || k==AUTO_LIGHT_ALWAYS){
  if(!auto_only(p,k==AUTO_LIGHT?"|enabled||brightness||toggle|":"|enabled||toggle|"))return false;
  if(auto_has(auto_get(p,"toggle"))){uint8_t toggle;if(!button || !auto_flag(p,"toggle",&toggle) || !toggle || auto_has(auto_get(p,"enabled")))return false;a->value=2;}
  else if(!auto_flag(p,"enabled",&a->value))return false;
  a->speed=255;
  if(k==AUTO_LIGHT && auto_has(auto_get(p,"brightness"))){if(!auto_integer(p,"brightness",&n,0,100))return false;a->speed=n;}
  return true;
 }
 return false;
}
static void auto_action_fields_mp(ph_writer *w,const automation_action_t *a){
 unsigned k=a->kind<=AUTO_ACTION_MAX?a->kind:0;
 phw_text(w,"action",k==AUTO_LIGHT_EFFECT&&a->value==255?"light_effect.next":auto_names[k]);phw_begin(w,"params",false);
 if(k==AUTO_FAN_MODE)phw_text(w,"mode",a->value?"auto":"manual");
 else if(k==AUTO_OUTLET||k==AUTO_FAN){
  if(a->value==2)phw_bool(w,"toggle",true);else phw_bool(w,"on",a->value);
  if(k==AUTO_OUTLET&&a->target<6)phw_uint(w,"index",a->target);
  if(k==AUTO_FAN&&a->speed)phw_uint(w,"speed",a->speed);
 }else if(k==AUTO_WAKE_ALARM){phw_uint(w,"duration_seconds",a->value);if(a->speed)phw_uint(w,"tone",a->speed);}
 else if(k==AUTO_DISPLAY)phw_uint(w,"brightness",a->value);
 else if(k==AUTO_LIGHT||k==AUTO_CHILD_LOCK||k==AUTO_INDICATOR||k==AUTO_LIGHT_ALWAYS){
  if(a->value==2)phw_bool(w,"toggle",true);else phw_bool(w,"enabled",a->value);
  if(k==AUTO_LIGHT&&a->speed<=100)phw_uint(w,"brightness",a->speed);
 }else if(k==AUTO_LIGHT_EFFECT&&a->value!=255)phw_uint(w,"effect",a->value);
 phw_end(w);
}
static void auto_task_fields_mp(ph_writer *w,const automation_task_t *t){
 auto_action_fields_mp(w,&t->action);phw_uint(w,"id",t->id);phw_bool(w,"enabled",t->enabled);
 phw_text(w,"trigger",t->once==3?"power":t->once==2?"countdown":t->once?"once":"weekly");
 if(t->once==3){
  phw_text(w,"comparison",t->days==0?"above":"below");phw_uint(w,"power_milliwatts",t->at);phw_uint(w,"seconds",t->last);
 }else{phw_uint(w,"days",t->days);phw_uint(w,"minute",t->minute);phw_uint(w,"at",t->at);phw_uint(w,"last",t->last);}
 const char *states[]={"waiting","executed","missed","busy","migration_disabled","cancelled"};phw_text(w,"state",states[t->state<=AUTO_CANCELLED?t->state:0]);
#ifdef FEATURE_DEVICE_A1
 if(t->once==2){phw_uint(w,"seconds",t->at);phw_uint(w,"remaining_seconds",auto_remaining(t));}
#endif
}
bool automation_mp(const char *name,const ph_value *params,ph_writer *w,const char **code){
 if(strncmp(name,"schedules.",10)&&strncmp(name,"buttons.",8))return false;
 auto_init();*code="invalid_params";ph_value p=params?*params:(ph_value){.kind=PHV_MISSING};
 automation_config_t *c=malloc(sizeof(*c));if(!c){*code="busy";phw_begin(w,NULL,false);phw_end(w);return true;}
 mico_rtos_lock_mutex(&automation_lock);automation_load(c);bool save=false;
 int id=0,index=0,slot=-1;bool task_output=false,output_started=false;
#ifdef FEATURE_DEVICE_A1
 if(!auto_countdown_boot(c)){*code="storage_error";goto done;}
#endif
 if(!strcmp(name,"schedules.get")||!strcmp(name,"buttons.get")){
  bool buttons=!strcmp(name,"buttons.get");int capacity=buttons?AUTOMATION_BUTTON_SLOTS:AUTOMATION_TASKS,offset=0,limit=8;
  if(!auto_only(p,"|offset||limit|")||(auto_has(auto_get(p,"offset"))&&!auto_integer(p,"offset",&offset,0,capacity))||(auto_has(auto_get(p,"limit"))&&!auto_integer(p,"limit",&limit,1,8)))goto done;
  phw_begin(w,NULL,false);output_started=true;phw_begin(w,"items",true);unsigned count=0;index=capacity;
  for(int i=0;i<capacity;i++)if(buttons?c->buttons[i/34][i%34].kind:c->tasks[i].id)count++;
  for(int i=offset;i<capacity&&limit;i++)if(buttons?c->buttons[i/34][i%34].kind:c->tasks[i].id){
   phw_begin(w,NULL,false);
   if(buttons){auto_action_fields_mp(w,&c->buttons[i/34][i%34]);phw_uint(w,"key",i/34);phw_uint(w,"clicks",i%34<30?i%34+1:0);phw_uint(w,"seconds",i%34>=30?i%34-29:0);}
   else auto_task_fields_mp(w,&c->tasks[i]);
   phw_end(w);index=i+1;limit--;
  }
  phw_end(w);phw_uint(w,"next",index);phw_uint(w,"capacity",capacity);phw_uint(w,"count",count);*code="ok";
 }else if(!strcmp(name,"schedules.put")){
  if(!auto_only(p,"|id||enabled||trigger||days||minute||at||seconds||action||params||comparison||power_milliwatts|"))goto done;
  if(auto_has(auto_get(p,"id"))&&!auto_integer(p,"id",&id,1,65535))goto done;
  for(unsigned i=0;i<AUTOMATION_TASKS;i++){if(id&&c->tasks[i].id==id){slot=i;break;}if(!id&&!c->tasks[i].id&&slot<0)slot=i;}
  if(slot<0){*code=id?"not_found":"capacity";goto done;}
  automation_task_t task;memset(&task,0,sizeof(task));task.id=id;
  if(!auto_flag(p,"enabled",&task.enabled))goto done;ph_value trigger=auto_get(p,"trigger");
#ifdef AUTOMATION_TC1
  if(phv_equal_text(&trigger,"power")){
   if(!auto_only(p,"|id||enabled||trigger||comparison||power_milliwatts||seconds||action||params|"))goto done;
   unsigned count=0;for(unsigned i=0;i<AUTOMATION_TASKS;i++)if(c->tasks[i].once==3&&c->tasks[i].id&&c->tasks[i].id!=id)count++;
   if(count>=PH_POWER_RULES_MAX){*code="capacity";goto done;}
   task.once=3;ph_value comparison=auto_get(p,"comparison");
   if(phv_equal_text(&comparison,"above"))task.days=PH_POWER_ABOVE;
   else if(phv_equal_text(&comparison,"below"))task.days=PH_POWER_BELOW;else goto done;
   if(!auto_integer(p,"power_milliwatts",&index,0,PH_POWER_LIMIT_MW))goto done;task.at=(uint32_t)index;
   if(!auto_integer(p,"seconds",&index,0,PH_POWER_DURATION_MAX))goto done;task.last=(uint32_t)index;
  }else
#endif
#ifdef FEATURE_DEVICE_A1
  if(phv_equal_text(&trigger,"countdown")){task.once=2;if(!auto_integer(p,"seconds",&index,1,AUTOMATION_COUNTDOWN_MAX))goto done;task.at=index;}
  else
#endif
  if(phv_equal_text(&trigger,"once")){task.once=1;if(!auto_integer(p,"at",&index,1577836800,2147483647))goto done;task.at=index;}
  else if(phv_equal_text(&trigger,"weekly")){if(!auto_integer(p,"days",&index,1,127))goto done;task.days=index;if(!auto_integer(p,"minute",&index,0,1439))goto done;task.minute=index;}
  else goto done;
  if(task.once!=3&&(auto_has(auto_get(p,"comparison"))||auto_has(auto_get(p,"power_milliwatts"))))goto done;
  if(!auto_action_parse(p,&task.action,false,false)||task.action.kind==AUTO_NONE)goto done;
  if(task.once==3&&(task.action.kind!=AUTO_OUTLET||task.action.value>1))goto done;
  if(task.once==2&&(task.action.kind!=AUTO_FAN||task.action.value!=0||task.action.speed))goto done;
  if(!id){for(unsigned tries=0;tries<65535;tries++){id=c->next_id++;if(!id)continue;bool used=false;for(unsigned i=0;i<AUTOMATION_TASKS;i++)used|=c->tasks[i].id==id;if(!used)break;}task.id=id;}
  c->tasks[slot]=task;task_output=true;save=true;
 }else if(!strcmp(name,"schedules.delete")||!strcmp(name,"schedules.enable")){
  bool enable=!strcmp(name,"schedules.enable");uint8_t enabled=0;
  if(!auto_only(p,enable?"|id||enabled|":"|id|")||!auto_integer(p,"id",&id,1,65535)||(enable&&!auto_flag(p,"enabled",&enabled)))goto done;
  for(unsigned i=0;i<AUTOMATION_TASKS;i++)if(c->tasks[i].id==id){slot=i;break;}
  if(slot<0){*code="not_found";goto done;}
  if(enable){c->tasks[slot].enabled=enabled;c->tasks[slot].state=AUTO_WAITING;}else memset(&c->tasks[slot],0,sizeof(c->tasks[slot]));save=true;
 }else if(!strcmp(name,"buttons.set")){
#if !defined(FEATURE_DEVICE_A1) && !defined(AUTOMATION_TC1)
  *code="unsupported";goto done;
#endif
  int key=0,clicks=0,seconds=0;
  if(!auto_only(p,"|key||clicks||seconds||action||params|")||!auto_integer(p,"key",&key,0,
#ifdef FEATURE_DEVICE_A1
  2
#else
  0
#endif
  )||(auto_has(auto_get(p,"clicks"))&&!auto_integer(p,"clicks",&clicks,0,30))||(auto_has(auto_get(p,"seconds"))&&!auto_integer(p,"seconds",&seconds,0,4))|| (!!clicks==!!seconds))goto done;
  automation_action_t action;if(!auto_action_parse(p,&action,true,false))goto done;c->buttons[key][clicks?clicks-1:seconds+29]=action;save=true;
 }else *code="unsupported";
 if(save){c->revision++;*code=automation_save(c)?"accepted":"storage_error";
#ifdef AUTOMATION_TC1
  if(!strcmp(*code,"accepted")&&slot>=0)auto_power_reset((unsigned)id);
#endif
#ifdef FEATURE_DEVICE_A1
  if(!strcmp(*code,"accepted")&&slot>=0){automation_task_t *t=&c->tasks[slot];countdowns[slot].id=0;if(t->id&&t->once==2&&t->enabled){countdowns[slot].id=t->id;countdowns[slot].started=mico_rtos_get_time();}}
#endif
 }
 done:
 if(!output_started)phw_begin(w,NULL,false);
 if(task_output&&slot>=0)auto_task_fields_mp(w,&c->tasks[slot]);
 phw_end(w);mico_rtos_unlock_mutex(&automation_lock);free(c);return true;
}
static bool auto_execute(const automation_action_t *a,bool button){
#ifdef FEATURE_DEVICE_A1
 return a1_automation_execute(a,button);
#elif defined(AUTOMATION_TC1)
 return tc1_automation_execute(a,button);
#else
 return m1_automation_execute(a,button);
#endif
}
void automation_button(unsigned key,unsigned clicks,unsigned seconds){
 if(key>=AUTOMATION_KEYS || clicks>30 || seconds>4 || (!!clicks==!!seconds))return;
 auto_init();automation_action_t a;
#ifdef AUTOMATION_TC1
 /* Read one mapping instead of allocating all 128 schedules on a key press. */
 mico_rtos_lock_mutex(&automation_lock);
 bool loaded=tc1_automation_load_button(key,clicks?clicks-1:seconds+29,&a);
 mico_rtos_unlock_mutex(&automation_lock);
 if(!loaded){service_event("info","button","gesture");service_event("warn","command","rejected");return;}
#else
 automation_config_t *c=malloc(sizeof(*c));if(!c)return;
 mico_rtos_lock_mutex(&automation_lock);automation_load(c);
 a=c->buttons[key][clicks?clicks-1:seconds+29];
 mico_rtos_unlock_mutex(&automation_lock);free(c);
#endif
 service_event("info","button","gesture");auto_execute(&a,true);
}
#ifdef FEATURE_DEVICE_A1
static void auto_countdown_poll(void) {
 /* Avoid configuration allocation/copy on every worker tick with no timer. */
 mico_rtos_lock_mutex(&automation_lock);bool active=!countdown_ready;
 for(unsigned i=0;i<AUTOMATION_TASKS;i++)active|=countdowns[i].id!=0;
 mico_rtos_unlock_mutex(&automation_lock);if(!active)return;
 automation_config_t *c=malloc(sizeof(*c));if(!c)return;
 for(unsigned i=0;i<AUTOMATION_TASKS;i++){
  mico_rtos_lock_mutex(&automation_lock);automation_load(c);
  if(!auto_countdown_boot(c)){mico_rtos_unlock_mutex(&automation_lock);break;}
  automation_task_t *t=&c->tasks[i];
  if(t->once!=2 || !t->id || !t->enabled || countdowns[i].id!=t->id || auto_remaining(t)){
   mico_rtos_unlock_mutex(&automation_lock);continue;
  }
  uint16_t id=t->id;automation_action_t action=t->action;t->enabled=0;t->state=AUTO_EXECUTED;c->revision++;
  bool saved=automation_save(c);if(saved)countdowns[i].id=0;
  mico_rtos_unlock_mutex(&automation_lock);
  if(!saved){service_event("error","schedule","storage_error");break;}
  bool ok=auto_execute(&action,false);service_event(ok?"info":"warn","schedule",ok?"executed":"busy");
  if(!ok){mico_rtos_lock_mutex(&automation_lock);automation_load(c);
   if(c->tasks[i].id==id && !c->tasks[i].enabled && c->tasks[i].state==AUTO_EXECUTED){c->tasks[i].state=AUTO_BUSY;c->revision++;automation_save(c);}
   mico_rtos_unlock_mutex(&automation_lock);
  }
 }
 free(c);
}
#endif
/* A small selected-rule snapshot, not a 128-schedule configuration allocation.
 * Execution rechecks the exact rule while holding the business write gate. */
#ifdef AUTOMATION_TC1
static void auto_power_poll(void){
 static uint32_t last_poll;static bool polled;
 uint32_t now=mico_rtos_get_time();if(polled&&(uint32_t)(now-last_poll)<1000u)return;
 polled=true;last_poll=now;
 ph_power_rule rules[PH_POWER_RULES_MAX];unsigned count=0;uint32_t mw=0;
 bool sample=tc1_automation_power_sample(&mw);
 mico_rtos_lock_mutex(&automation_lock);
 if(!tc1_automation_load_power_rules(rules,&count)){
  for(unsigned i=0;i<PH_POWER_RULES_MAX;i++)power_states[i].trigger.timing=false;
  mico_rtos_unlock_mutex(&automation_lock);return;
 }
 for(unsigned i=0;i<PH_POWER_RULES_MAX;i++){
  bool found=false;for(unsigned j=0;j<count;j++)found|=rules[j].id==power_states[i].rule.id;
  if(!found)memset(&power_states[i],0,sizeof(power_states[i]));
 }
 mico_rtos_unlock_mutex(&automation_lock);
 for(unsigned j=0;j<count;j++){
  mico_rtos_lock_mutex(&automation_lock);int slot=-1;
  for(unsigned i=0;i<PH_POWER_RULES_MAX;i++)if(power_states[i].rule.id==rules[j].id){slot=(int)i;break;}
  if(slot<0)for(unsigned i=0;i<PH_POWER_RULES_MAX;i++)if(!power_states[i].rule.id){slot=(int)i;break;}
  if(slot<0){mico_rtos_unlock_mutex(&automation_lock);break;}
  auto_power_state *s=&power_states[slot];
  if(memcmp(&s->rule,&rules[j],sizeof(s->rule))){memset(s,0,sizeof(*s));s->rule=rules[j];}
  bool due=ph_power_trigger_due(&s->trigger,&s->rule,now,sample,mw);
  mico_rtos_unlock_mutex(&automation_lock);if(!due)continue;
  bool ok=tc1_automation_power_execute(&rules[j]);
  mico_rtos_lock_mutex(&automation_lock);
  if(ok&&!memcmp(&s->rule,&rules[j],sizeof(s->rule)))ph_power_trigger_commit(&s->trigger);
  mico_rtos_unlock_mutex(&automation_lock);
  if(ok)service_event("info","schedule","executed");
 }
}
#endif
void automation_poll(bool valid){
 auto_init();
#ifdef AUTOMATION_TC1
 auto_power_poll();
#endif
#ifdef FEATURE_DEVICE_A1
 auto_countdown_poll();
#endif
 uint32_t now=(uint32_t)time(NULL),ms=mico_rtos_get_time();if(!valid || now<1577836800){armed=false;return;}
 bool jump=armed && ((int64_t)now-previous-(uint32_t)(ms-previous_ms)/1000>2 || (int64_t)now-previous-(uint32_t)(ms-previous_ms)/1000< -2);
 if(!armed || jump){previous=now;previous_ms=ms;if(now>highwater)highwater=now;armed=true;return;}previous_ms=ms;if(now<=previous){previous=now;return;}
 automation_config_t *c=malloc(sizeof(*c));if(!c)return;
 /* Reboot runs last. Every due record is committed before its action. */
 for(unsigned pass=0;pass<2;pass++){
  unsigned last_id=0;
  for(unsigned count=0;count<AUTOMATION_TASKS;count++){
   mico_rtos_lock_mutex(&automation_lock);automation_load(c);int selected=-1;unsigned id=65536;uint32_t occurrence=0;
   for(unsigned i=0;i<AUTOMATION_TASKS;i++){
    automation_task_t *t=&c->tasks[i];if(t->once>=2 || !t->id || !t->enabled || t->id<=last_id || t->id>=id || ((t->action.kind==AUTO_REBOOT)!=(pass==1)))continue;
    uint32_t at=t->at;
    if(!t->once){int64_t local=(int64_t)now+c->timezone*60;int64_t day=local/86400;unsigned weekday=(day+3)%7;if(!(t->days&(1u<<weekday)))continue;at=day*86400+t->minute*60-c->timezone*60;}
    if(at>now || at<=t->last || (!t->once && (at<=previous || at<=highwater)))continue;
    selected=i;id=t->id;occurrence=at;
   }
   if(selected<0){mico_rtos_unlock_mutex(&automation_lock);break;}last_id=id;
   automation_task_t *t=&c->tasks[selected];automation_action_t action=t->action;t->last=occurrence;if(t->once)t->enabled=0;
   bool timely=now-occurrence<=60 && occurrence>highwater;t->state=timely?AUTO_EXECUTED:AUTO_MISSED;c->revision++;bool saved=automation_save(c);mico_rtos_unlock_mutex(&automation_lock);
   if(!saved){service_event("error","schedule","storage_error");break;}
   if(!timely){service_event("warn","schedule","missed");continue;}
   bool ok=auto_execute(&action,false);service_event(ok?"info":"warn","schedule",ok?"executed":"busy");
   if(!ok){mico_rtos_lock_mutex(&automation_lock);automation_load(c);if(c->tasks[selected].id==id && c->tasks[selected].last==occurrence){c->tasks[selected].state=AUTO_BUSY;c->revision++;automation_save(c);}mico_rtos_unlock_mutex(&automation_lock);}
  }
 }
 free(c);previous=now;if(now>highwater)highwater=now;
}

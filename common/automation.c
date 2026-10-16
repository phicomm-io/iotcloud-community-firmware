#include "automation.h"
#include "mico.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>
static mico_mutex_t automation_lock;
static bool automation_ready;
static uint32_t previous, highwater, previous_ms;
static bool armed;
void automation_defaults(automation_config_t *c){memset(c,0,sizeof(*c));c->magic=AUTOMATION_MAGIC;c->timezone=480;c->next_id=1;
#ifdef AUTOMATION_TC1
/* TC1 has no default custom key action. Fixed recovery is separate. */
#elif defined(FEATURE_DEVICE_A1)
c->buttons[0][0].kind=AUTO_FAN;c->buttons[0][0].value=2;
#endif
}
static void auto_init(void){if(!automation_ready)automation_ready=mico_rtos_init_mutex(&automation_lock)==kNoErr;}
static void auto_text(json_object *o,const char *k,const char *s){json_build_object_add(o,k,json_object_new_string(s));}
static bool auto_integer(json_object *o,const char *k,int *v,int lo,int hi){json_object *p=json_object_object_get(o,k);if(!p || !json_object_is_type(p,json_type_int))return false;*v=json_object_get_int(p);return *v>=lo && *v<=hi;}
static bool auto_flag(json_object *o,const char *k,uint8_t *v){json_object *p=json_object_object_get(o,k);if(!p || !json_object_is_type(p,json_type_boolean))return false;*v=json_object_get_boolean(p);return true;}
static bool auto_only(json_object *o,const char *allowed){json_object_object_foreach(o,k,v){char n[80];snprintf(n,sizeof(n),"|%s|",k);if(!v || !strstr(allowed,n))return false;}return true;}
static const char *auto_names[]={"none","outlet.set","fan.set","light.set","child_lock.set","indicator.set","display.set","system.reboot"};
static bool auto_supported(unsigned k){
#ifdef AUTOMATION_TC1
 return k==AUTO_NONE || k==AUTO_OUTLET || k==AUTO_CHILD_LOCK || k==AUTO_INDICATOR || k==AUTO_REBOOT;
#elif defined(FEATURE_DEVICE_A1)
 return k==AUTO_NONE || k==AUTO_FAN || k==AUTO_LIGHT || k==AUTO_CHILD_LOCK || k==AUTO_REBOOT;
#else
 return k==AUTO_NONE || k==AUTO_DISPLAY || k==AUTO_REBOOT;
#endif
}
static bool auto_action_parse(json_object *o,automation_action_t *a,bool button){
 memset(a,0,sizeof(*a));json_object *name=json_object_object_get(o,"action"),*p=json_object_object_get(o,"params");
 if(!auto_only(o,"|action||params|") || !name || !json_object_is_type(name,json_type_string) || !p || !json_object_is_type(p,json_type_object))return false;
 unsigned k;for(k=0;k<=AUTO_REBOOT;k++)if(!strcmp(auto_names[k],json_object_get_string(name)))break;
 if(k>AUTO_REBOOT || !auto_supported(k))return false;a->kind=k;int n=0;
 if(k==AUTO_NONE || k==AUTO_REBOOT)return auto_only(p,"");
 if(k==AUTO_FAN || k==AUTO_OUTLET){
  if(!auto_only(p,k==AUTO_FAN?"|on||speed||toggle|":"|on||index||toggle|"))return false;
  uint8_t value;json_object *toggle=json_object_object_get(p,"toggle");
  if(toggle){if(!button || !auto_flag(p,"toggle",&value) || !value || json_object_object_get(p,"on"))return false;a->value=2;}
  else {if(!auto_flag(p,"on",&value))return false;a->value=value;}
  a->target=255;a->speed=0;
  if(json_object_object_get(p,"index")){if(!auto_integer(p,"index",&n,0,5))return false;a->target=n;}
  if(json_object_object_get(p,"speed")){if(!auto_integer(p,"speed",&n,1,100))return false;a->speed=n;}
  return true;
 }
 if(k==AUTO_DISPLAY){if(!auto_only(p,"|brightness|") || !auto_integer(p,"brightness",&n,0,100) || n%25)return false;a->value=n;return true;}
 if(k==AUTO_LIGHT){if(!auto_only(p,"|enabled||brightness|") || !auto_flag(p,"enabled",&a->value) || !auto_integer(p,"brightness",&n,0,100))return false;a->speed=n;return true;}
 return auto_only(p,"|enabled|") && auto_flag(p,"enabled",&a->value);
}
static json_object *auto_action_json(const automation_action_t *a){
 json_object *o=json_object_new_object(),*p=json_object_new_object();unsigned k=a->kind<=AUTO_REBOOT?a->kind:0;auto_text(o,"action",auto_names[k]);
 if(k==AUTO_OUTLET || k==AUTO_FAN){
  if(a->value==2)json_build_object_add(p,"toggle",json_object_new_boolean(true));else json_build_object_add(p,"on",json_object_new_boolean(a->value));
  if(k==AUTO_OUTLET && a->target<6)json_build_object_add(p,"index",json_object_new_int(a->target));if(k==AUTO_FAN && a->speed)json_build_object_add(p,"speed",json_object_new_int(a->speed));
 }else if(k==AUTO_DISPLAY)json_build_object_add(p,"brightness",json_object_new_int(a->value));
 else if(k==AUTO_LIGHT){json_build_object_add(p,"enabled",json_object_new_boolean(a->value));json_build_object_add(p,"brightness",json_object_new_int(a->speed));}
 else if(k==AUTO_CHILD_LOCK || k==AUTO_INDICATOR)json_build_object_add(p,"enabled",json_object_new_boolean(a->value));
 json_build_object_add(o,"params",p);return o;
}
static json_object *auto_task_json(const automation_task_t *t){json_object *o=auto_action_json(&t->action);json_build_object_add(o,"id",json_object_new_int(t->id));json_build_object_add(o,"enabled",json_object_new_boolean(t->enabled));auto_text(o,"trigger",t->once?"once":"weekly");json_build_object_add(o,"days",json_object_new_int(t->days));json_build_object_add(o,"minute",json_object_new_int(t->minute));json_build_object_add(o,"at",json_object_new_int(t->at));json_build_object_add(o,"last",json_object_new_int(t->last));const char *states[]={"waiting","executed","missed","busy","migration_disabled"};auto_text(o,"state",states[t->state<=AUTO_MIGRATED_DISABLED?t->state:0]);return o;}
json_object *automation_command(const char *name,json_object *p,const char **code,bool *handled){
 *handled=!strncmp(name,"schedules.",10) || !strncmp(name,"buttons.",8);if(!*handled)return NULL;
 auto_init();*code="invalid_params";automation_config_t *c=malloc(sizeof(*c));if(!c){*code="busy";return NULL;}
 mico_rtos_lock_mutex(&automation_lock);automation_load(c);json_object *out=NULL;bool save=false;
 int id=0,index=0;int slot=-1;
 if(!strcmp(name,"schedules.get")){
  int offset=0,limit=8;if(!auto_only(p,"|offset||limit|") || (json_object_object_get(p,"offset")&&!auto_integer(p,"offset",&offset,0,AUTOMATION_TASKS)) || (json_object_object_get(p,"limit")&&!auto_integer(p,"limit",&limit,1,8)))goto done;
  out=json_object_new_object();json_object *a=json_object_new_array();unsigned count=0;for(unsigned i=0;i<AUTOMATION_TASKS;i++)if(c->tasks[i].id)count++;
  for(unsigned i=offset;i<AUTOMATION_TASKS && limit;i++)if(c->tasks[i].id){json_build_array_add(a,auto_task_json(&c->tasks[i]));index=i+1;limit--;}
  if(!index)index=AUTOMATION_TASKS;json_build_object_add(out,"items",a);json_build_object_add(out,"next",json_object_new_int(index));json_build_object_add(out,"capacity",json_object_new_int(AUTOMATION_TASKS));json_build_object_add(out,"count",json_object_new_int(count));*code="ok";
 }else if(!strcmp(name,"schedules.put")){
  if(!auto_only(p,"|id||enabled||trigger||days||minute||at||action||params|"))goto done;
  if(json_object_object_get(p,"id")&&!auto_integer(p,"id",&id,1,65535))goto done;
  for(unsigned i=0;i<AUTOMATION_TASKS;i++){if(id && c->tasks[i].id==id){slot=i;break;}if(!id && !c->tasks[i].id && slot<0)slot=i;}
  if(slot<0){*code=id?"not_found":"capacity";goto done;}
  automation_task_t task;memset(&task,0,sizeof(task));task.id=id;
  if(!auto_flag(p,"enabled",&task.enabled))goto done;json_object *trigger=json_object_object_get(p,"trigger");if(!trigger || !json_object_is_type(trigger,json_type_string))goto done;
  const char *t=json_object_get_string(trigger);
  if(!strcmp(t,"once")){task.once=1;if(!auto_integer(p,"at",&index,1577836800,2147483647))goto done;task.at=index;}
  else if(!strcmp(t,"weekly")){if(!auto_integer(p,"days",&index,1,127))goto done;task.days=index;if(!auto_integer(p,"minute",&index,0,1439))goto done;task.minute=index;}else goto done;
  json_object *a=json_object_new_object();json_object *an=json_object_object_get(p,"action"),*ap=json_object_object_get(p,"params");if(an)json_build_object_add(a,"action",json_object_get(an));if(ap)json_build_object_add(a,"params",json_object_get(ap));bool valid=auto_action_parse(a,&task.action,false) && task.action.kind!=AUTO_NONE;json_object_put(a);if(!valid)goto done;
  if(!id){for(unsigned tries=0;tries<65535;tries++){id=c->next_id++;if(!id)continue;bool used=false;for(unsigned i=0;i<AUTOMATION_TASKS;i++)used|=c->tasks[i].id==id;if(!used)break;}task.id=id;}
  c->tasks[slot]=task;out=auto_task_json(&task);save=true;
 }else if(!strcmp(name,"schedules.delete") || !strcmp(name,"schedules.enable")){
  bool enable=!strcmp(name,"schedules.enable");uint8_t enabled=0;if(!auto_only(p,enable?"|id||enabled|":"|id|") || !auto_integer(p,"id",&id,1,65535) || (enable&&!auto_flag(p,"enabled",&enabled)))goto done;
  for(unsigned i=0;i<AUTOMATION_TASKS;i++)if(c->tasks[i].id==id){slot=i;break;}if(slot<0){*code="not_found";goto done;}
  if(enable){c->tasks[slot].enabled=enabled;c->tasks[slot].state=AUTO_WAITING;}else memset(&c->tasks[slot],0,sizeof(c->tasks[slot]));save=true;
 }else if(!strcmp(name,"buttons.get")){
  int offset=0,limit=8;if(!auto_only(p,"|offset||limit|") || (json_object_object_get(p,"offset")&&!auto_integer(p,"offset",&offset,0,68)) || (json_object_object_get(p,"limit")&&!auto_integer(p,"limit",&limit,1,8)))goto done;
  out=json_object_new_object();json_object *a=json_object_new_array();unsigned count=0;index=68;
  for(unsigned n=0;n<68;n++)if(c->buttons[n/34][n%34].kind)count++;
  for(unsigned n=offset;n<68 && limit;n++)if(c->buttons[n/34][n%34].kind){unsigned k=n/34,i=n%34;json_object *v=auto_action_json(&c->buttons[k][i]);json_build_object_add(v,"key",json_object_new_int(k));json_build_object_add(v,"clicks",json_object_new_int(i<30?i+1:0));json_build_object_add(v,"seconds",json_object_new_int(i>=30?i-29:0));json_build_array_add(a,v);index=n+1;limit--;}
  json_build_object_add(out,"items",a);json_build_object_add(out,"next",json_object_new_int(index));json_build_object_add(out,"capacity",json_object_new_int(68));json_build_object_add(out,"count",json_object_new_int(count));*code="ok";
 }else if(!strcmp(name,"buttons.set")){
#ifndef FEATURE_DEVICE_A1
#ifndef AUTOMATION_TC1
  *code="unsupported";goto done;
#endif
#endif
  int key=0,clicks=0,seconds=0;
  if(!auto_only(p,"|key||clicks||seconds||action||params|") || !auto_integer(p,"key",&key,0,
#ifdef FEATURE_DEVICE_A1
  1
#else
  0
#endif
  ) || (json_object_object_get(p,"clicks")&&!auto_integer(p,"clicks",&clicks,0,30)) || (json_object_object_get(p,"seconds")&&!auto_integer(p,"seconds",&seconds,0,4)) || (!!clicks==!!seconds))goto done;
  json_object *a=json_object_new_object(),*an=json_object_object_get(p,"action"),*ap=json_object_object_get(p,"params");if(an)json_build_object_add(a,"action",json_object_get(an));if(ap)json_build_object_add(a,"params",json_object_get(ap));automation_action_t action;bool valid=auto_action_parse(a,&action,true);json_object_put(a);if(!valid)goto done;c->buttons[key][clicks?clicks-1:seconds+29]=action;save=true;
 }else *code="unsupported";
 if(save){c->revision++;*code=automation_save(c)?"accepted":"storage_error";}
 done:mico_rtos_unlock_mutex(&automation_lock);free(c);return out;
}
static bool auto_execute(const automation_action_t *a,bool button){
 if(!a->kind)return true;json_object *q=auto_action_json(a);
 if(button){
  json_object *status=service_query("status.get"),*s=json_object_object_get(status,"data");
  json_object *locked=json_object_object_get(s,"child_lock");if(locked && json_object_get_boolean(locked)){json_object_put(status);json_object_put(q);service_event("info","button","child_locked");return false;}
  if(a->value==2 && (a->kind==AUTO_FAN || a->kind==AUTO_OUTLET)){
   bool on=false;if(a->kind==AUTO_FAN){json_object *v=json_object_object_get(s,"requested_fan_on");on=v&&json_object_get_boolean(v);}else{json_object *states=json_object_object_get(s,"socket_states");if(states)for(unsigned i=0;i<json_object_array_length(states);i++)if(a->target==255 || a->target==i)on|=json_object_get_int(json_object_array_get_idx(states,i))!=0;}
   json_object *p=json_object_object_get(q,"params");json_object_object_del(p,"toggle");json_build_object_add(p,"on",json_object_new_boolean(!on));
  }json_object_put(status);
 }
 json_object *r=service_command(q);json_object_put(q);const char *code=r?json_object_get_string(json_object_object_get(r,"code")):"busy";bool ok=!strcmp(code,"ok")||!strcmp(code,"accepted");if(r)json_object_put(r);return ok;
}
void automation_button(unsigned key,unsigned clicks,unsigned seconds){if(key>1 || clicks>30 || seconds>4 || (!!clicks==!!seconds))return;auto_init();automation_config_t *c=malloc(sizeof(*c));if(!c)return;mico_rtos_lock_mutex(&automation_lock);automation_load(c);automation_action_t a=c->buttons[key][clicks?clicks-1:seconds+29];mico_rtos_unlock_mutex(&automation_lock);free(c);service_event("info","button","gesture");auto_execute(&a,true);}
void automation_poll(bool valid){
 auto_init();uint32_t now=(uint32_t)time(NULL),ms=mico_rtos_get_time();if(!valid || now<1577836800){armed=false;return;}
 bool jump=armed && ((int64_t)now-previous-(uint32_t)(ms-previous_ms)/1000>2 || (int64_t)now-previous-(uint32_t)(ms-previous_ms)/1000< -2);
 if(!armed || jump){previous=now;previous_ms=ms;if(now>highwater)highwater=now;armed=true;return;}previous_ms=ms;if(now<=previous){previous=now;return;}
 automation_config_t *c=malloc(sizeof(*c));if(!c)return;
 /* Reboot runs last. Every due record is committed before its action. */
 for(unsigned pass=0;pass<2;pass++){
  unsigned last_id=0;
  for(unsigned count=0;count<AUTOMATION_TASKS;count++){
   mico_rtos_lock_mutex(&automation_lock);automation_load(c);int selected=-1;unsigned id=65536;uint32_t occurrence=0;
   for(unsigned i=0;i<AUTOMATION_TASKS;i++){
    automation_task_t *t=&c->tasks[i];if(!t->id || !t->enabled || t->id<=last_id || t->id>=id || ((t->action.kind==AUTO_REBOOT)!=(pass==1)))continue;
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

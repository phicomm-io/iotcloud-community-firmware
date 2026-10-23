#include "a1.h"
#include "fresh_air.h"
#include <math.h>
static mico_mutex_t output_lock;
static bool inhibited, pwm_running, feedback_ready;
static volatile uint32_t pulses;
static uint32_t sample_at, previous_pulses, control_at, revision;
static fresh_config_t config;
static fresh_snapshot_t published, current;
static feature_pid_t controller;
static unsigned automatic_speed, next_fresh, next_primary;
static void feedback_edge(void *arg) { (void)arg; ++pulses; }
static OSStatus output(unsigned speed) {
    float duty=fresh_duty(&config.mapping,speed);
    OSStatus err=kNoErr;
    if(pwm_running) {err=MicoPwmStop(MICO_PWM_1);pwm_running=false;}
    if(err==kNoErr && (duty==0 || duty==100)) {
        err=duty ? MicoGpioOutputHigh(MICO_GPIO_9):MicoGpioOutputLow(MICO_GPIO_9);
        if(err==kNoErr)err=MicoGpioInitialize(MICO_GPIO_9,OUTPUT_PUSH_PULL);
    } else if(err==kNoErr) {
        err=MicoPwmInitialize(MICO_PWM_1,25000,duty);
        if(err==kNoErr)err=MicoPwmStart(MICO_PWM_1);
        pwm_running=err==kNoErr;
    }
    if(err!=kNoErr) {
        MicoPwmStop(MICO_PWM_1);pwm_running=false;
        if(config.mapping.active_low)MicoGpioOutputHigh(MICO_GPIO_9);else MicoGpioOutputLow(MICO_GPIO_9);
        MicoGpioInitialize(MICO_GPIO_9,OUTPUT_PUSH_PULL);
        speed=0;duty=fresh_duty(&config.mapping,0);current.reason=3;
    }
    current.applied=speed;current.duty=duty;current.ready=err==kNoErr;
    return err;
}
OSStatus fresh_boot(void) {
    OSStatus err=mico_rtos_init_mutex(&output_lock);if(err!=kNoErr)return err;
    fresh_config_get(&config);
    return output(0);
}
void fresh_shutdown(void) {
    mico_rtos_lock_mutex(&output_lock); inhibited=true;
    output(0);MicoGpioOutputHigh(MICO_GPIO_19);MicoPwmStop(MICO_PWM_3);
    current.primary=0;current.reason=1;
    mico_rtos_lock_mutex(&a1_lock);published=current;mico_rtos_unlock_mutex(&a1_lock);
    mico_rtos_unlock_mutex(&output_lock);
}
void fresh_init(void) {
    OSStatus err=MicoGpioDisableIRQ(MICO_GPIO_10);
    if(err==kNoErr)err=MicoGpioInitialize(MICO_GPIO_10,INPUT_PULL_UP);
    if(err==kNoErr)err=MicoGpioEnableIRQ(MICO_GPIO_10,IRQ_TRIGGER_FALLING_EDGE,feedback_edge,NULL);
    feedback_ready=err==kNoErr;sample_at=mico_rtos_get_time();
}
void fresh_prepare(bool master,unsigned desired,unsigned applied,uint32_t now,unsigned *next) {
    fresh_config_t c;fresh_config_get(&c);
    mico_rtos_lock_mutex(&output_lock); /* Held through both output operations. */
    bool changed=revision!=c.revision;
    if(changed) {
        output(0);config=c;output(0);revision=c.revision;
        memset(&controller,0,sizeof(controller));control_at=now-5000;
    }
    current.requested_on=c.on;current.requested_speed=c.speed;current.automatic=c.automatic;
    current.reason=0;current.fallback=false;
    unsigned target=c.speed;
    if(c.automatic && master && c.on && !inhibited) {
        if((uint32_t)(now-control_at)>=5000) {
            float values[4]={0},co2=0;bool on;
            bool valid=feature_fresh_values(c.linkage.statistic,c.linkage.metrics,values,&co2);
            if(valid) feature_control_co2(&c.linkage,&c.co2,&controller,values,co2,now,true,automatic_speed,&on,&automatic_speed);
            else {memset(&controller,0,sizeof(controller));automatic_speed=c.fallback_speed;
                if(automatic_speed<c.linkage.min_speed)automatic_speed=c.linkage.min_speed;
                if(automatic_speed>c.linkage.max_speed)automatic_speed=c.linkage.max_speed;}
            current.fallback=!valid;control_at=now;
        } else current.fallback=published.fallback;
        target=automatic_speed;
    }
    if(target>100)target=100;
    current.target=target;
    if(!master || inhibited) {
        *next=next_fresh=0;current.reason=1;
        memset(&controller,0,sizeof(controller));control_at=now-5000;
    } else {
        if(!c.on)target=0;
        fresh_pair(&c.mapping,desired,target,feature_primary_ceiling(),applied,current.applied,next,&next_fresh);
        if(!c.on)next_fresh=0;
        if(next_fresh<target)current.reason=2;
        if(changed){next_fresh=0;current.reason=4;}
    }
    next_primary=*next;
    /* Lower fresh output before lowering the primary output. */
    if(next_fresh<current.applied)output(next_fresh);
}
void fresh_finish(unsigned primary,uint32_t now) {
    current.primary=primary;
    unsigned cap=config.mapping.coupling?primary*100/config.mapping.boost_ratio:primary*config.mapping.cap_ratio/100;
    if(next_fresh>cap)next_fresh=cap;
    if(primary!=next_primary)current.reason=3;
    if(next_fresh!=current.applied || !current.ready)output(next_fresh);
    uint32_t count=pulses,elapsed=now-sample_at;
    if(elapsed>=1000) {
        current.rpm=feedback_ready?(uint64_t)(count-previous_pulses)*60000u/elapsed/config.mapping.pulses_per_rev:0;
        current.pulses=count;previous_pulses=count;sample_at=now;
    }
    mico_rtos_lock_mutex(&a1_lock);published=current;mico_rtos_unlock_mutex(&a1_lock);
    mico_rtos_unlock_mutex(&output_lock);
}
void fresh_snapshot(fresh_snapshot_t *out) {
    mico_rtos_lock_mutex(&a1_lock);*out=published;mico_rtos_unlock_mutex(&a1_lock);
}
static void number(json_object *o,const char *k,double n){json_build_object_add(o,k,json_object_new_double(n));}
static json_object *encode(const fresh_config_t *c,bool linkage) {
    json_object *o=linkage?feature_fresh_encode(c):json_object_new_object();if(!o)return NULL;
    json_build_object_add(o,"revision",json_object_new_int(c->revision));
    if(!linkage) {
        number(o,"pulses_per_rev",c->mapping.pulses_per_rev);
        number(o,"coupling",c->mapping.coupling);number(o,"cap_ratio",c->mapping.cap_ratio);number(o,"boost_ratio",c->mapping.boost_ratio);
        json_build_object_add(o,"active_low",json_object_new_boolean(c->mapping.active_low));
        number(o,"a",c->mapping.a);number(o,"b",c->mapping.b);number(o,"pwm_min",c->mapping.pwm_min);number(o,"pwm_max",c->mapping.pwm_max);
        json_build_object_add(o,"on",json_object_new_boolean(c->on));number(o,"speed",c->speed);
        json_build_object_add(o,"mode",json_object_new_string(c->automatic?"auto":"manual"));
    }
    return o;
}
json_object *fresh_command(const char *action,json_object *p,const char **code) {
    fresh_config_t c;fresh_config_get(&c);*code="invalid_params";
    bool linkage=!strncmp(action,"fresh_air.linkage.",18);
    bool get=!strcmp(action,"fresh_air.config.get")||!strcmp(action,"fresh_air.linkage.get");
    bool config_set=!strcmp(action,"fresh_air.config.set");
    bool control=!strcmp(action,"fresh_air.set");
    if(get){json_object_object_foreach(p,key,value){(void)key;(void)value;return NULL;}*code="ok";return encode(&c,linkage);}
    if(!config_set && !control && strcmp(action,"fresh_air.linkage.set")){*code="unknown_action";return NULL;}
    json_object *rv=json_object_object_get(p,"revision");
    if(!control && (!rv || !json_object_is_type(rv,json_type_int) || json_object_get_int(rv)<0))return NULL;
    if(rv && (!json_object_is_type(rv,json_type_int)||json_object_get_int(rv)<0))return NULL;
    if(rv && (uint32_t)json_object_get_int(rv)!=c.revision){*code="conflict";return NULL;}
    if(linkage) {if(!feature_fresh_decode(&c,p))return NULL;}
    else {json_object_object_foreach(p,k,v) {
        if(!strcmp(k,"revision"))continue;
        if(control && !strcmp(k,"mode")) {
            if(!json_object_is_type(v,json_type_string))return NULL;
            const char *s=json_object_get_string(v);
            if(!strcmp(s,"auto"))c.automatic=1;else if(!strcmp(s,"manual"))c.automatic=0;else return NULL;
            continue;
        }
        if((control && !strcmp(k,"on")) || (config_set && !strcmp(k,"active_low"))) {
            if(!json_object_is_type(v,json_type_boolean))return NULL;
            if(control)c.on=json_object_get_boolean(v);else c.mapping.active_low=json_object_get_boolean(v);
            continue;
        }
        if(!json_object_is_type(v,json_type_int)&&!json_object_is_type(v,json_type_double))return NULL;
        double n=json_object_get_double(v);if(!isfinite(n))return NULL;
        if(control && !strcmp(k,"speed")){if(n<0||n>100||(int)n!=n)return NULL;c.speed=n;}
        else if(config_set && !strcmp(k,"a"))c.mapping.a=n;
        else if(config_set && !strcmp(k,"b"))c.mapping.b=n;
        else if(config_set && !strcmp(k,"pwm_min"))c.mapping.pwm_min=n;
        else if(config_set && !strcmp(k,"pwm_max"))c.mapping.pwm_max=n;
        else if(config_set && (!strcmp(k,"coupling")||!strcmp(k,"cap_ratio")||!strcmp(k,"boost_ratio")||!strcmp(k,"pulses_per_rev"))) {
            if(n<0||n>500||(int)n!=n)return NULL;
            if(!strcmp(k,"coupling")){if(n>1)return NULL;c.mapping.coupling=n;}
            else if(!strcmp(k,"cap_ratio"))c.mapping.cap_ratio=n;else if(!strcmp(k,"boost_ratio"))c.mapping.boost_ratio=n;else c.mapping.pulses_per_rev=n;
        } else return NULL;
    }}
    if(!fresh_mapping_valid(&c.mapping))return NULL;
    *code=fresh_config_save(&c);if(strcmp(*code,"accepted"))return NULL;
    fresh_config_get(&c);return encode(&c,linkage);
}

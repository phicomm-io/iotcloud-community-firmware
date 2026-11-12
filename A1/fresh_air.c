#include "a1.h"
#include "fresh_air.h"
#include "../common/service_mp.h"
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
    mico_rtos_lock_mutex(&a1_lock);bool changed=memcmp(&published,&current,sizeof(current))!=0;published=current;mico_rtos_unlock_mutex(&a1_lock);
    if(changed)service_cache_changed(SERVICE_CACHE_STATUS);
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
    bool remap=memcmp(&config.mapping,&c.mapping,sizeof(c.mapping))!=0;
    if(changed) {
        if(remap)output(0);
        config=c;if(remap)output(0);revision=c.revision;
        memset(&controller,0,sizeof(controller));control_at=now-5000;
    }
    current.requested_on=c.on;current.requested_speed=c.speed;current.automatic=c.automatic;
    current.reason=0;current.fallback=false;
    unsigned target=c.speed;
    if(c.automatic && master && c.on && !inhibited) {
        if((uint32_t)(now-control_at)>=5000) {
            float values[4]={0},co2=0;bool on;
            bool valid=feature_fresh_values(c.linkage.statistic,c.linkage.metrics,values,&co2);
            if(valid) {
                float integral=controller.integral;
                feature_control_co2(&c.linkage,&c.co2,&controller,values,co2,now,true,current.applied,&on,&automatic_speed);
                if(published.reason==2 && automatic_speed>current.applied && controller.integral>integral)controller.integral=integral;
            }
            else {memset(&controller,0,sizeof(controller));automatic_speed=c.fallback_speed;
                if(automatic_speed<c.linkage.min_speed)automatic_speed=c.linkage.min_speed;
                if(automatic_speed>c.linkage.max_speed)automatic_speed=c.linkage.max_speed;}
            current.fallback=!valid;control_at=now;
        } else current.fallback=published.fallback;
        target=automatic_speed;
    }
    if(target>100)target=100;
    if(!c.on){target=0;memset(&controller,0,sizeof(controller));control_at=now-5000;}
    current.target=target;
    if(!master || inhibited) {
        *next=next_fresh=0;current.reason=1;
        memset(&controller,0,sizeof(controller));control_at=now-5000;
    } else {
        if(!c.on)target=0;
        fresh_pair(&c.mapping,desired,target,feature_primary_ceiling(),applied,current.applied,next,&next_fresh);
        if(!c.on)next_fresh=0;
        if(next_fresh<target)current.reason=2;
        if(remap){next_fresh=0;current.reason=4;}
    }
    next_primary=*next;
    /* Lower fresh output before lowering the primary output. */
    if(next_fresh<current.applied)output(next_fresh);
}
void fresh_finish(unsigned primary,uint32_t now) {
    current.primary=primary;
    unsigned cap=fresh_limit(&config.mapping,primary);
    if(next_fresh>cap)next_fresh=cap;
    if(primary!=next_primary){next_fresh=0;current.reason=3;}
    if(next_fresh!=current.applied || !current.ready)output(next_fresh);
    uint32_t count=pulses,elapsed=now-sample_at;
    if(elapsed>=1000) {
        current.rpm=feedback_ready?(uint64_t)(count-previous_pulses)*60000u/elapsed/config.mapping.pulses_per_rev:0;
        current.pulses=count;previous_pulses=count;sample_at=now;
    }
    mico_rtos_lock_mutex(&a1_lock);bool changed=memcmp(&published,&current,sizeof(current))!=0;published=current;mico_rtos_unlock_mutex(&a1_lock);
    if(changed)service_cache_changed(SERVICE_CACHE_STATUS);
    mico_rtos_unlock_mutex(&output_lock);
}
void fresh_snapshot(fresh_snapshot_t *out) {
    mico_rtos_lock_mutex(&a1_lock);*out=published;mico_rtos_unlock_mutex(&a1_lock);
}
#include "fresh_air_mp.inc"

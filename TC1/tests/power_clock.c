/* Stateful pinned SDK clock + complete production sampler; no DUT or SDK edit. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "TC1/user_power.h"
#define MCU_CLOCK_HZ 100000000u
#define INPUT_PULL_UP 0
#define IRQ_TRIGGER_FALLING_EDGE 0
#define POWER 15
#define power_log(...) ((void)0)
static struct { uint16_t power_coefficient_x100; uint32_t p_count_1_day_ago; } config={1710,0}, *user_config=&config;
static const uint32_t NR_NS_PER_SECOND=1000000000u;
static uint64_t nsclock_nsec;
static uint32_t nsclock_sec,prev_cycles,ns_divisor=1;
static uint32_t cycles,ticks;
static bool inject_irq,inject_reader;
static unsigned ns_reads,delivered;
static tc1_power_sample_t nested;
static void (*power_irq)(void *);
static uint32_t mico_rtos_get_time(void) {
    uint32_t captured=ticks;
    if(inject_irq) {
        inject_irq=false;cycles+=100;++delivered;power_irq(NULL);
    }
    return captured;
}
static uint32_t platform_get_cycle_count(void) {
    uint32_t captured=cycles;
    if(inject_reader) {
        // An older SDK user such as gettimeofday was preempted after its cycle
        // capture. The MQTT reader must not reenter that shared accumulator.
        inject_reader=false;cycles+=100;PowerSampleRead(&nested);
    }
    return captured;
}
#include "sdk-clock.inc"
static uint64_t mico_nanosecond_clock_value(void) { ++ns_reads;return platform_get_nanosecond_clock_value(); }
static void MicoGpioInitialize(unsigned pin,unsigned mode) { assert(pin==POWER && mode==INPUT_PULL_UP); }
static void MicoGpioEnableIRQ(unsigned pin,unsigned mode,void (*handler)(void *),void *arg) {
    assert(pin==POWER && mode==IRQ_TRIGGER_FALLING_EDGE && !arg);power_irq=handler;
}
#include "clock-power.inc"

int main(void) {
    PowerInit();
    cycles=10000000;ticks=100;power_irq(NULL);
    cycles=120000000;ticks=1200;power_irq(NULL);
    cycles=130000000;ticks=1300;
    tc1_power_sample_t s;PowerSampleRead(&s);assert(s.valid && s.age_ms==100);
    // The existing GPIO IRQ may interrupt the new task-side tick reader.
    inject_irq=true;PowerSampleRead(&s);
    assert(!inject_irq && delivered==1 && s.valid && s.age_ms==100 && nsclock_sec==1);
    // The other ordering: an old thread already entered the SDK nsclock, then
    // was suspended while the new MQTT reader executes. No global clock race.
    inject_reader=true;
    uint64_t ns=mico_nanosecond_clock_value();
    assert(!inject_reader && nested.valid && nested.age_ms==100);
    assert(ns==1300001000 && nsclock_sec==1); // no false 32-bit CPU-cycle wrap
    assert(ns_reads==4); // Only the 3 existing pulse calls + old SDK thread.
    ticks=21199;PowerSampleRead(&s);assert(s.valid && s.age_ms==19999);
    ticks=21200;PowerSampleRead(&s);assert(!s.valid && s.age_ms==20000);
    ticks=1300;PowerSampleRead(&s);assert(!s.valid && s.age_ms==UINT32_MAX);
    // A real new sample on either side of the tick wrap remains coherent.
    ticks=UINT32_MAX-500;power_sample_commit(125,ticks);
    ticks+=1234;PowerSampleRead(&s);assert(s.valid && s.age_ms==1234);
    // A reconnect may follow unobserved whole tick wraps: only new IRQ data
    // can restore validity, while process boot/sequence live in the owner.
    PowerSampleInvalidate();ticks+=100;PowerSampleRead(&s);assert(!s.valid && s.age_ms==UINT32_MAX);
    power_sample_commit(125,ticks);PowerSampleRead(&s);assert(s.valid && !s.age_ms);
    assert(ns_reads==4);
    puts("actual SDK clock + GPIO IRQ/old-thread interleavings + RTOS age + rollover/reconnect stale latch PASS");
    return 0;
}

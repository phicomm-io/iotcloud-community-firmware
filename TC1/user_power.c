#include "http_server/web_log.h"
#include "TimeUtils.h"

#include "mico.h"
#include "main.h"
#include "mqtt_server/user_mqtt_client.h"
#include "user_power.h"
#include <math.h>
#include <string.h>

uint32_t p_count = 0;
volatile uint32_t power_session_pulses;
PowerRecord power_record = {1, {0}};
char power_record_str[1101] = {0};
float real_time_power = 0;

/* One IRQ writer, one MQTT owner. The sample age uses the IRQ-safe RTOS
 * millisecond tick on both sides; thread code must not enter the SDK's mutable
 * nsclock accumulator used by the existing pulse measurement. */
static uint32_t power_sample_version, power_sample_bits, power_sample_at;
static uint32_t power_sample_expired;
static void power_sample_commit(float raw, uint32_t at_ms) {
    uint32_t bits, version=__atomic_load_n(&power_sample_version,__ATOMIC_SEQ_CST);
    memcpy(&bits,&raw,sizeof(bits));
    __atomic_store_n(&power_sample_version,version+1u,__ATOMIC_SEQ_CST);
    __atomic_store_n(&power_sample_bits,bits,__ATOMIC_SEQ_CST);
    __atomic_store_n(&power_sample_at,at_ms,__ATOMIC_SEQ_CST);
    __atomic_store_n(&power_sample_version,version+2u,__ATOMIC_SEQ_CST);
}
void PowerSampleInvalidate(void) {
    /* A reconnect may follow arbitrarily many 32-bit tick wraps. Require a
     * new genuine calculation; neither process boot nor sequence is reset. */
    __atomic_store_n(&power_sample_expired,
        __atomic_load_n(&power_sample_version,__ATOMIC_SEQ_CST),__ATOMIC_SEQ_CST);
}
void PowerSampleRead(tc1_power_sample_t *out) {
    if(!out)return;
    *out=(tc1_power_sample_t){0};
    for(unsigned attempt=0;attempt<3;attempt++) {
        uint32_t before=__atomic_load_n(&power_sample_version,__ATOMIC_SEQ_CST);
        if(!before || (before&1u))continue;
        uint32_t bits=__atomic_load_n(&power_sample_bits,__ATOMIC_SEQ_CST);
        uint32_t at=__atomic_load_n(&power_sample_at,__ATOMIC_SEQ_CST);
        if(before!=__atomic_load_n(&power_sample_version,__ATOMIC_SEQ_CST))continue;
        /* Once expired, the same sample cannot revive on tick rollover. A
         * saturated invalid age also covers the unknown gap on reconnect. */
        if(before==__atomic_load_n(&power_sample_expired,__ATOMIC_SEQ_CST)) {
            out->age_ms=UINT32_MAX;return;
        }
        uint32_t age=mico_rtos_get_time()-at;
        float raw;memcpy(&raw,&bits,sizeof(raw));
        out->watts=raw/10.0f;
        out->age_ms=age;
        out->valid=age<20000 && isfinite(out->watts) && out->watts>=0;
        if(age>=20000)__atomic_store_n(&power_sample_expired,before,__ATOMIC_SEQ_CST);
        return;
    }
}

/* One local automation reader. Count CF pulses even at zero load, instead of
 * treating the MQTT sample's session invalidation or lack of a new IRQ as a
 * stale network sensor. A five-interval average limits pulse quantization.
 * An interrupted worker interval cancels continuity, never invents zero load. */
static struct {
    uint32_t at,count,pulses[5],elapsed[5];
    unsigned used,cursor;
    uint16_t coefficient;
    bool ready;
} local_power;
bool PowerLocalSampleRead(uint32_t *milliwatts) {
    if(!milliwatts||!local_power.ready)return false;
    uint32_t now=mico_rtos_get_time(),count=__atomic_load_n(&power_session_pulses,__ATOMIC_RELAXED);
    uint32_t elapsed=now-local_power.at,delta=count-local_power.count;
    uint16_t coefficient=user_config->power_coefficient_x100;
    if(elapsed<1000)return false;
    local_power.at=now;local_power.count=count;
    if(elapsed>=3000||coefficient!=local_power.coefficient){
        local_power.used=local_power.cursor=0;local_power.coefficient=coefficient;return false;
    }
    unsigned slot=local_power.cursor++%5;
    local_power.pulses[slot]=delta;local_power.elapsed[slot]=elapsed;
    if(local_power.used<5)++local_power.used;
    if(local_power.used<5)return false;
    uint64_t pulses=0;uint32_t duration=0;
    for(unsigned i=0;i<5;i++){pulses+=local_power.pulses[i];duration+=local_power.elapsed[i];}
    uint64_t mw=(pulses*coefficient*1000u+duration/2u)/duration;
    if(coefficient<POWER_COEFFICIENT_MIN_X100||coefficient>POWER_COEFFICIENT_MAX_X100||mw>25000000u)return false;
    *milliwatts=(uint32_t)mw;return true;
}

float GetPowerCoefficient(void) {
    return (float) user_config->power_coefficient_x100 / POWER_COEFFICIENT_SCALE;
}

float PowerPulseCountToKwh(uint32_t pulse_count) {
    return GetPowerCoefficient() * pulse_count / 1000.0f / 36000.0f;
}

void SetPowerRecord(PowerRecord *pr, uint32_t pw) {
    pr->powers[(++pr->idx) % PW_NUM] = pw;
}

char *GetPowerRecord(int idx) {
    if (idx > power_record.idx) return "";
    idx = idx <= power_record.idx - PW_NUM ? 0 : idx;

    int i = idx > 0 ? idx : (power_record.idx - PW_NUM + 1);
    i = i < 0 ? 0 : i;
    size_t used=0; power_record_str[0]=0;
    int last=power_record.idx;
    for (; i <= last; i++) {
        int n=snprintf(power_record_str+used,sizeof(power_record_str)-used,"%s%lu",used?",":"",(unsigned long)power_record.powers[i%PW_NUM]);
        if(n<0 || (size_t)n>=sizeof(power_record_str)-used)break;
        used+=n;
    }
    return power_record_str;
}

uint64_t NS = 1000000000;
float n_1s = 0;       //在当前这一秒功率中断次数
uint64_t past_ns = 0; //系统运行的纳秒数
uint64_t irq_old = 0; //上次中断的时间(纳秒)

static void PowerIrqHandler(void *arg) {
    //警告! 不能在此函数里调用任何有关moloc()的操作
    if (p_count == 0) {
        p_count = user_config->p_count_1_day_ago;
    }

    p_count++;
    power_session_pulses++;

    //mico_time_get_time(&past_ns); //系统运行毫秒数
    past_ns = mico_nanosecond_clock_value(); //系统运行纳秒数
    uint64_t spend_ns = past_ns - irq_old;

    if (irq_old % NS + spend_ns <= NS) {
        n_1s += 1;
        irq_old = past_ns;
        return;
    }

    int n = (spend_ns - past_ns % NS) / NS;
    n_1s += (float) (NS - irq_old % NS) / spend_ns;
    real_time_power = GetPowerCoefficient() * n_1s;
    SetPowerRecord(&power_record, (int) real_time_power);

    int i = 0;
    /* Only the last ring capacity is observable; a long idle gap must not
     * spend minutes filling history in interrupt context. */
    if(n>PW_NUM){power_record.idx+=n-PW_NUM;n=PW_NUM;}
    for (; i < n; i++) {
        real_time_power = GetPowerCoefficient() * NS / spend_ns;
        SetPowerRecord(&power_record, (int) real_time_power);
    }
    irq_old = past_ns;
    n_1s = (float) (past_ns % NS) / spend_ns;
    power_sample_commit(real_time_power,mico_rtos_get_time());
}

void PowerInit(void) {
    power_log("PowerInit");
    memset(&local_power,0,sizeof(local_power));
    if(MicoGpioInitialize(POWER, INPUT_PULL_UP)!=kNoErr ||
       MicoGpioEnableIRQ(POWER, IRQ_TRIGGER_FALLING_EDGE, PowerIrqHandler, NULL)!=kNoErr)return;
    local_power.at=mico_rtos_get_time();local_power.count=__atomic_load_n(&power_session_pulses,__ATOMIC_RELAXED);
    local_power.coefficient=user_config->power_coefficient_x100;local_power.ready=true;
}


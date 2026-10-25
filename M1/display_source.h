/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef M1_DISPLAY_SOURCE_H
#define M1_DISPLAY_SOURCE_H
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#define M1_DISPLAY_MAGIC 0x4d314453u
#define M1_DISPLAY_LANES 2u
enum { M1_DISPLAY_LOCAL, M1_DISPLAY_MQTT, M1_DISPLAY_API, M1_DISPLAY_NONE };
enum { M1_DISPLAY_CO2, M1_DISPLAY_PM25, M1_DISPLAY_HCHO,
       M1_DISPLAY_TEMPERATURE, M1_DISPLAY_HUMIDITY, M1_DISPLAY_PRESSURE,
       M1_DISPLAY_PM10, M1_DISPLAY_TVOC, M1_DISPLAY_POWER, M1_DISPLAY_ENERGY,
       M1_DISPLAY_FAN_SPEED_PERCENT, M1_DISPLAY_CUSTOM,
       M1_DISPLAY_METRIC_COUNT };
enum { M1_DISPLAY_PPM, M1_DISPLAY_UG_M3, M1_DISPLAY_MG_M3,
       M1_DISPLAY_CELSIUS, M1_DISPLAY_PERCENT, M1_DISPLAY_PA,
       M1_DISPLAY_PPB, M1_DISPLAY_WATTS, M1_DISPLAY_KWH, M1_DISPLAY_UNITLESS,
       M1_DISPLAY_UNIT_COUNT };
enum { M1_DISPLAY_COLOR_FIXED, M1_DISPLAY_COLOR_THRESHOLDS };
enum { M1_DISPLAY_OK, M1_DISPLAY_DISABLED, M1_DISPLAY_MISSING, M1_DISPLAY_STALE,
       M1_DISPLAY_OFFLINE, M1_DISPLAY_INVALID, M1_DISPLAY_OUT_OF_RANGE, M1_DISPLAY_UNSUPPORTED };

/* Appended storage only. The pre-v9 CO2 profile retains its own bytes and
 * remains authoritative until the user explicitly configures this profile. */
typedef struct {
    char device_id[13];
    uint8_t source, metric, unit;
    uint16_t scale;
    uint8_t decimals, ttl_seconds, color_mode, color, reserved[2];
    float low, high;
} m1_display_lane_config_t;
typedef struct {
    uint32_t magic, revision;
    uint8_t configured, selected, double_click, reserved;
    m1_display_lane_config_t lanes[M1_DISPLAY_LANES];
} m1_display_config_t;
typedef struct {
    float value;
    uint16_t number;
    uint8_t format, color, ttl;
    uint8_t reason;
    uint32_t age_ms;
    bool valid, age_known;
} m1_display_value_t;
typedef struct {
    uint16_t number;
    uint8_t format, color, ttl;
    bool confirmed, showing;
} m1_display_readback_t;
typedef struct {
    uint32_t revision;
    bool configured, selected, double_click, present;
} m1_display_switch_config_t;

static inline uint8_t m1_display_metric_unit(unsigned metric)
{
    static const uint8_t units[]={M1_DISPLAY_PPM,M1_DISPLAY_UG_M3,M1_DISPLAY_MG_M3,
        M1_DISPLAY_CELSIUS,M1_DISPLAY_PERCENT,M1_DISPLAY_PA,
        M1_DISPLAY_UG_M3,M1_DISPLAY_PPB,M1_DISPLAY_WATTS,M1_DISPLAY_KWH,M1_DISPLAY_PERCENT,M1_DISPLAY_UNITLESS};
    return metric<M1_DISPLAY_METRIC_COUNT?units[metric]:M1_DISPLAY_UNIT_COUNT;
}
static inline bool m1_display_metric_value_valid(unsigned metric,float value)
{
    if(!isfinite(value))return false;
    if(metric==M1_DISPLAY_TEMPERATURE)return value>=-40 && value<=125;
    if(value<0)return false;
    if(metric==M1_DISPLAY_CO2)return value>0 && value<=65535;
    if(metric==M1_DISPLAY_HUMIDITY || metric==M1_DISPLAY_FAN_SPEED_PERCENT)return value<=100;
    return true;
}
static inline void m1_display_defaults(m1_display_config_t *c)
{
    *c=(m1_display_config_t){.magic=M1_DISPLAY_MAGIC};
    for(unsigned i=0;i<M1_DISPLAY_LANES;i++)c->lanes[i]=(m1_display_lane_config_t){
        .source=M1_DISPLAY_NONE,.metric=M1_DISPLAY_CUSTOM,.unit=M1_DISPLAY_UNITLESS,
        .scale=1,.ttl_seconds=20,.color_mode=M1_DISPLAY_COLOR_FIXED};
}
static inline bool m1_display_config_valid(const m1_display_config_t *c)
{
    if(c->magic!=M1_DISPLAY_MAGIC || c->revision>INT32_MAX || c->configured>1 ||
       c->selected>1 || c->double_click>1 || c->reserved)return false;
    for(unsigned i=0;i<M1_DISPLAY_LANES;i++){
        const m1_display_lane_config_t *l=&c->lanes[i];
        if(l->source>M1_DISPLAY_NONE || l->metric>=M1_DISPLAY_METRIC_COUNT ||
           l->unit>=M1_DISPLAY_UNIT_COUNT || l->decimals>2 ||
           (l->scale!=1&&l->scale!=1000) || !l->ttl_seconds || l->ttl_seconds>60 ||
           l->color_mode>M1_DISPLAY_COLOR_THRESHOLDS || l->color>3 ||
           l->reserved[0] || l->reserved[1] || !isfinite(l->low) ||
           !isfinite(l->high) || (l->color_mode==M1_DISPLAY_COLOR_THRESHOLDS && l->low>=l->high) || l->device_id[12])return false;
        if(l->metric!=M1_DISPLAY_CUSTOM && l->unit!=m1_display_metric_unit(l->metric))return false;
        if(l->source==M1_DISPLAY_LOCAL && l->metric>M1_DISPLAY_PRESSURE)return false;
        if(l->source==M1_DISPLAY_MQTT && l->metric==M1_DISPLAY_CUSTOM)return false;
        if(l->source==M1_DISPLAY_NONE && (l->metric!=M1_DISPLAY_CUSTOM || l->unit!=M1_DISPLAY_UNITLESS))return false;
        for(unsigned j=0;j<13;j++){
            unsigned char x=l->device_id[j];
            if(l->source==M1_DISPLAY_MQTT){
                if(j<12 && !((x>='0'&&x<='9')||(x>='a'&&x<='f')))return false;
            }else if(x)return false;
        }
    }
    return true;
}
/* The glass has three unsigned digits. A value that cannot be represented
 * invalidates this lane instead of wrapping, clamping or changing its unit. */
static inline bool m1_display_encode_value(const m1_display_lane_config_t *c,
                                          unsigned slot,float value,uint8_t ttl,
                                          uint8_t color,m1_display_value_t *out)
{
    *out=(m1_display_value_t){0};
    if(slot>=M1_DISPLAY_LANES || !ttl || !isfinite(value) || value<0 ||
       c->decimals>2 || (c->scale!=1&&c->scale!=1000) || color>3)return false;
    float factor=c->decimals==2?100.0f:c->decimals==1?10.0f:1.0f;
    float number=value/c->scale*factor;
    if(!isfinite(number) || number>=999.5f)return false;
    out->value=value;out->number=(uint16_t)(number+0.5f);
    out->format=c->decimals?(uint8_t)(3-c->decimals):0;
    /* Only the exact original unit/metric in its original slot may keep
     * those etched labels visible. Every other combination hides them. */
    if(c->scale==1 && ((slot==0 && c->metric==M1_DISPLAY_HCHO && c->unit==M1_DISPLAY_MG_M3) ||
       (slot==1 && c->metric==M1_DISPLAY_PM25 && c->unit==M1_DISPLAY_UG_M3)))out->format|=4;
    out->color=color;out->ttl=ttl;out->valid=true;return true;
}
/* Policy belongs to the Wi-Fi MCU. The display MCU receives only segments,
 * colors and a short lease; none of its original sensor values are replaced. */
static inline bool m1_display_source_selected(unsigned mode,int manual,
                                               uint32_t elapsed,unsigned seconds)
{
    if(manual>=0)return manual!=0;
    if(mode==1)return true;
    return mode==2 && seconds && elapsed%((uint32_t)seconds*2000)<(uint32_t)seconds*1000;
}
static inline void m1_display_source_encode(uint8_t args[7],uint16_t ppm,uint8_t ttl,
                                            uint8_t slot,uint16_t low,uint16_t high)
{
    uint16_t number=ppm;uint8_t decimal=0;
    if(ppm>=10000){number=ppm/100;decimal=2;}
    else if(ppm>=1000){number=ppm/10;decimal=1;}
    args[0]=number>>8;args[1]=number;args[2]=decimal;
    args[3]=ppm>=high?1:ppm>=low?3:2;
    args[4]=ttl;args[5]=slot;args[6]=0;
}
#endif

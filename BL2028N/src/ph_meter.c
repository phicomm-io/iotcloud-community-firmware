/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "meter.h"
#include <math.h>
#include <string.h>
/* HLW8032 User Manual Rev 1.5, registers and equations pp. 12-17.
 * Independent implementation over OpenBeken's UART HAL; no other framework. */
static uint32_t u24(const unsigned char *p) { return ((uint32_t)p[0]<<16)|((uint32_t)p[1]<<8)|p[2]; }
void ph_meter_init(ph_meter *m, double energy) { memset(m,0,sizeof(*m));m->energy_kwh=energy; }
unsigned ph_meter_fields(const ph_meter *m, uint32_t t) {
    unsigned fields=m->fields;
    for(unsigned i=0;i<3;++i)if((uint32_t)(t-m->field_at[i])>=PH_METER_FRESH_MS)fields&=~(1u<<i);
    return fields;
}
static void frame(ph_meter *m, uint32_t t, const double c[3]) {
    const unsigned char *p=m->buffer; unsigned state=p[0], flags=p[20];
    ++m->frames;m->frame_at=t;
    if(state==0xaa || (state!=0x55 && (state&1))) {m->fields=0;m->pulse_ready=false;return;}
    for(unsigned i=0;i<3;++i) {
        unsigned overflow=8u>>i, updated=64u>>i;uint32_t parameter=u24(p+2+i*6), measurement=u24(p+5+i*6);
        if(state!=0x55 && (state&overflow)) {m->raw[i]=m->value[i]=0;m->field_at[i]=t;m->fields|=1u<<i;}
        else if(flags&updated) {
            if(!parameter || !measurement){m->fields&=~(1u<<i);continue;}
            double raw=(double)parameter/measurement, value=raw*c[i];
            if(!isfinite(value)||value<0||value>(i==0?500:i==1?100:25000)){m->fields&=~(1u<<i);continue;}
            m->raw[i]=raw;m->value[i]=value;m->field_at[i]=t;m->fields|=1u<<i;
        }
    }
    /* PF is a 17-bit counter: update-register bit 7 toggles on each 16-bit
     * rollover. Baseline on boot, stale input, IC reset or implausible jumps. */
    uint32_t pulses=((uint32_t)(flags&0x80)<<9)|((uint32_t)p[21]<<8)|p[22];
    uint32_t elapsed=t-m->pulse_at, delta=(pulses-m->pulse)&0x1ffffu;
    double per_pulse=u24(p+14)*c[2]/3.6e12;
    if(m->pulse_ready && elapsed<PH_METER_FRESH_MS && per_pulse>0) {
        double energy=delta*per_pulse, maximum=25000.0*(elapsed+100)/3.6e9+2*per_pulse;
        if(energy<=maximum && isfinite(energy))m->energy_kwh+=energy;
        else ++m->discontinuities;
    }
    m->pulse=pulses;m->pulse_at=t;m->pulse_ready=true;
}
bool ph_meter_byte(ph_meter *m, unsigned char b, uint32_t t, const double c[3]) {
    ++m->received_bytes;
    if((uint32_t)(t-m->byte_at)>150)m->used=0;
    m->byte_at=t;m->buffer[m->used++]=b;
    if(m->used<24)return false;
    unsigned sum=0;for(unsigned i=2;i<23;++i)sum+=m->buffer[i];
    unsigned state=m->buffer[0];
    if(m->buffer[1]==0x5a && (state==0x55||state==0xaa||(state&0xf0)==0xf0) && (sum&255)==m->buffer[23]) {
        frame(m,t,c);m->used=0;return true;
    }
    ++m->bad_frames;memmove(m->buffer,m->buffer+1,23);m->used=23;return false;
}
void ph_meter_write(const ph_meter *m, ph_writer *w, uint32_t t, bool calibrated) {
    unsigned fields=ph_meter_fields(m,t);
    phw_begin(w,"metering",false);phw_text(w,"chip","HLW8032");phw_bool(w,"calibrated",calibrated);
    phw_bool(w,"valid",fields==7);phw_u32(w,"valid_fields",fields);
    const char *keys[]={"voltage","current","active_power"};
    for(unsigned i=0;i<3;++i){if(fields&(1u<<i))phw_number(w,keys[i],m->value[i]);else phw_null(w,keys[i]);}
    double apparent=m->value[0]*m->value[1];
    if((fields&3)==3)phw_number(w,"apparent_power",apparent);else phw_null(w,"apparent_power");
    if(fields==7 && apparent>0)phw_number(w,"power_factor",fmin(m->value[2]/apparent,1));else phw_null(w,"power_factor");
    phw_number(w,"energy_kwh",m->energy_kwh);phw_end(w);
}

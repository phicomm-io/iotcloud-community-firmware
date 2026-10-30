/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "device.h"
#include "../protocol/sensor_mp.h"
/* Existing account-scoped schema3 descriptors, separate from business status.
 * Only descriptors are retained; readings carry boot/sequence/age and expire. */
static inline void ph_meter_sensor_metadata(const ph_device *d,ph_writer *w){
    ph_sensor_metadata_begin(w,d->mac,d->boot,1);
    ph_sensor_descriptor_write(w,"voltage","Voltage","V");
    ph_sensor_descriptor_write(w,"current","Current","A");
    ph_sensor_descriptor_write(w,"power","Power","W");
    ph_sensor_descriptor_write(w,"energy","Energy","kWh");
    ph_sensor_finish(w);
}
static inline void ph_meter_sensor_state(const ph_device *d,ph_writer *w,uint32_t sequence,uint32_t now){
    const ph_meter *m=&d->meter;unsigned fields=ph_meter_fields(m,now);
    const char *id[]={"voltage","current","power"};
    ph_sensor_snapshot_begin(w,d->mac,d->boot,sequence,1);
    for(unsigned i=0;i<3;i++){
        uint32_t age=(m->fields&(1u<<i))?now-m->field_at[i]:UINT32_MAX;
        ph_sensor_reading_write(w,id[i],m->value[i],(fields&(1u<<i))!=0,age);
    }
    uint32_t age=m->frames?now-m->frame_at:UINT32_MAX;
    ph_sensor_reading_write(w,"energy",m->energy_kwh,m->frames&&age<PH_METER_FRESH_MS,age);
    ph_sensor_finish(w);
}

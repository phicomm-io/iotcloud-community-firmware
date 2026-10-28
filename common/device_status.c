/* Request-local frozen status encoder shared by HTTP and MQTT. */
#include "device_status.h"
#include "firmware_variant_defs.h"
#include <math.h>
#include <string.h>
#include "status_emit.inc"
static const char *device_mqtt_status(unsigned state)
{
    static const char *const names[]={"disconnected","connecting","config_error","auth_failed","waiting_acl","ready"};
    return state<sizeof(names)/sizeof(*names)?names[state]:names[0];
}
static void runtime_fields(status_sink *o,const device_runtime_t *s)
{
    if(s->reset_valid)emit_int(o,"last_reset_reason",s->reset_reason);
    emit_double(o,"uptime_ms",s->now_ms);
    emit_int(o,"uptime_seconds",s->now_ms/1000);
    if(!s->memory_valid)return;
    emit_int(o,"free_memory",s->free_memory);emit_int(o,"allocated_memory",s->allocated_memory);
    emit_int(o,"free_memory_chunks",s->free_memory_chunks);emit_int(o,"min_observed_free_memory",s->minimum_free);
}
#if defined(PHICOMM_APPLICATION_WATCHDOG)
#if !defined(PHICOMM_STATUS_MODEL) || PHICOMM_STATUS_MODEL == 0
static void device_filter_encode(status_sink *o,const a1_filter_storage_t *f,const a1_filter_stock_t *stock)
{
    unsigned remaining=stock->weighted?a1_filter_stock_remaining(stock):a1_filter_remaining(f);
    emit_string(o,"model",stock->weighted?"weighted_estimate":"runtime");
    emit_int(o,"weighted_usage",stock->usage);
    emit_int(o,"weighted_capacity",A1_FILTER_STOCK_CAPACITY);
    emit_int(o,"weighted_remaining_percent",a1_filter_stock_remaining(stock));
    emit_int(o,"time_remaining_percent",a1_filter_remaining(f));
    emit_double(o,"air_volume_m3_estimate",stock->air_litres/1000.0);
    emit_int(o,"used_seconds",f->used_seconds);emit_int(o,"lifetime_hours",f->lifetime_hours);
    emit_int(o,"remaining_percent",remaining);emit_bool(o,"replacement_due",remaining<=10);
}
static __attribute__((noinline)) void encode_a1(status_sink *o,const device_status_t *snapshot, const device_status_full_t *full)
{
    bool live = !full;

    emit_string(o,"model","a1");
    if (!live) {
        variant_fields(o);
        emit_int(o,"uptime_seconds",snapshot->now_ms/1000);
        emit_begin(o,"filter",false); device_filter_encode(o,&full->model.a1.filter,&full->model.a1.filter_stock); emit_end(o);
    } else emit_int(o, "uptime_seconds", snapshot->now_ms/1000);
    emit_string(o, "version", snapshot->identity.version); emit_string(o, "device_id", snapshot->identity.id); emit_string(o, "name", snapshot->identity.name);
    emit_bool(o, "wifi_trial_active", snapshot->network.wifi_trial_active);
    emit_string(o, "wifi_trial_status", snapshot->network.wifi_trial_status);
    if (!live) {
        emit_string(o, "ssid", full->network.ssid); emit_string(o, "ip", full->network.ip); emit_string(o, "ap_name", full->network.ap_name);
        emit_string(o, "mqtt_host", full->network.mqtt_host); emit_int(o, "mqtt_port", full->network.mqtt_port); emit_string(o, "mqtt_user", full->network.mqtt_user);
        emit_string(o, "mqtt_topic_mode", "tmcp");
        emit_string(o, "mqtt_protocol", "tmcp"); emit_int(o, "mqtt_protocol_version", 2);

        emit_bool(o, "web_password_set", full->network.web_password_set);
        emit_bool(o, "wifi_password_set", full->network.wifi_password_set);
        emit_bool(o, "mqtt_password_set", full->network.mqtt_password_set);
    }
    emit_string(o, "mqtt_status", device_mqtt_status(snapshot->network.mqtt_status));
    emit_bool(o, "mqtt_cleanup_warning", snapshot->network.mqtt_cleanup_warning);
    emit_bool(o, "wifi_connected", snapshot->network.wifi_connected); emit_bool(o, "mqtt_connected", snapshot->network.mqtt_connected);
    bool effective_on=snapshot->control.a1.effective_on; unsigned effective_speed=snapshot->control.a1.effective_speed;
    emit_string(o,"fan_mode",snapshot->control.a1.automatic?"auto":"manual");
    emit_string(o,"fan_state",!effective_on?"off":effective_speed?"running":"idle");
    emit_int(o,"auto_max_speed",snapshot->control.a1.auto_limit);
    emit_bool(o,"requested_fan_on",snapshot->control.a1.fan_on); emit_int(o,"requested_speed",snapshot->control.a1.speed);
    emit_bool(o, "ap_active", snapshot->network.ap_active); emit_bool(o, "fan_on", effective_on);
    emit_int(o, "speed", snapshot->control.a1.speed); emit_int(o, "percentage", effective_on ? effective_speed : 0);
    emit_int(o, "applied_speed", snapshot->control.a1.applied_speed); emit_bool(o, "fan_ready", snapshot->control.a1.fan_ready);
#ifdef A1_FRESH_AIR
    const fresh_snapshot_t *f=&snapshot->control.a1.fresh;
    emit_bool(o,"fresh_air_requested_on",f->requested_on);
    emit_int(o,"fresh_air_requested_speed",f->requested_speed);
    emit_string(o,"fresh_air_mode",f->automatic?"auto":"manual");
    emit_bool(o,"fresh_air_on",f->applied>0);
    emit_int(o,"fresh_air_target",f->target);emit_int(o,"fresh_air_speed",f->applied);
    emit_int(o,"fresh_air_primary_speed",f->primary);
    emit_int(o,"fresh_air_pwm_milli",(int)(f->duty*1000));
    emit_int(o,"fresh_air_rpm",f->rpm);emit_int(o,"fresh_air_pulses",f->pulses);
    emit_int(o,"fresh_air_limit_reason",f->reason);emit_bool(o,"fresh_air_ready",f->ready);
    emit_bool(o,"fresh_air_fallback",f->fallback);
#endif
    emit_bool(o, "touch_active", snapshot->control.a1.touch_active);
    emit_bool(o, "led_enabled", snapshot->control.a1.led_enabled); emit_bool(o, "led_visible", snapshot->control.a1.led_visible);
    emit_bool(o, "led_always_on", snapshot->control.a1.led_always_on); emit_int(o, "brightness", snapshot->control.a1.brightness);
    emit_int(o, "red", snapshot->control.a1.red); emit_int(o, "green", snapshot->control.a1.green); emit_int(o, "blue", snapshot->control.a1.blue);
    emit_bool(o, "child_lock", snapshot->control.a1.child_lock); emit_bool(o, "panel_ready", snapshot->control.a1.panel_ready);

    if(snapshot->captured & STATUS_SENSOR) {
        const feature_values_t *v=&snapshot->sensor.a1;
        emit_begin(o,"sensors",false);
        emit_bool(o,"valid",v->valid);
        emit_int(o,"statistic",v->statistic);
        emit_int(o,"sample_count",v->sample_count);
        emit_int(o,"co2_sample_count",v->co2_sample_count);
        emit_int(o,"configured_count",v->configured_count);
        const char *keys[]={"temperature","humidity","pm25","formaldehyde"};
        for(unsigned i=0;i<4;i++) {
            if(v->sample_count && isfinite(v->value[i]))emit_double(o,keys[i],v->value[i]);
            else emit_null(o,keys[i]);
        }
        bool co2_valid=v->co2_sample_count && isfinite(v->co2);
        emit_bool(o,"co2_valid",co2_valid);
        if(co2_valid)emit_double(o,"co2",v->co2);
        else emit_null(o,"co2");
        emit_end(o);
    }
    emit_bool(o, "config_dirty", snapshot->control.a1.config_dirty); emit_string(o, "save_error", snapshot->control.a1.save_error);

    emit_string(o, "ota_state", snapshot->ota.state); emit_string(o, "ota_error", snapshot->ota.error);
    emit_int(o, "ota_progress", snapshot->ota.progress); emit_bool(o, "ota_active", snapshot->ota.active);
    emit_bool(o, "reboot_pending", snapshot->ota.reboot_pending); emit_bool(o, "scan_busy", snapshot->ota.scan_busy);
}

#endif
#if !defined(PHICOMM_STATUS_MODEL) || PHICOMM_STATUS_MODEL == 1
static __attribute__((noinline)) void encode_m1(status_sink *o,const device_status_t *snapshot, const device_status_full_t *full)
{
    bool live = !full;

    uint32_t now = snapshot->now_ms; bool fresh = snapshot->sensor.m1.sample.received && (uint32_t)(now-snapshot->sensor.m1.sample.updated_ms)<M1_STALE_MS;
    emit_string(o,"model","m1");
    if (!live) {
        variant_fields(o);
        emit_int(o,"uptime_seconds",snapshot->now_ms/1000);
    } else emit_int(o, "uptime_seconds", snapshot->now_ms/1000);
    emit_string(o, "version", snapshot->identity.version); emit_string(o, "device_id", snapshot->identity.id); emit_string(o, "name", snapshot->identity.name);
    emit_bool(o, "wifi_trial_active", snapshot->network.wifi_trial_active);
    emit_string(o, "wifi_trial_status", snapshot->network.wifi_trial_status);
    if (!live) {
        emit_string(o, "ssid", full->network.ssid); emit_string(o, "ip", full->network.ip); emit_string(o, "ap_name", full->network.ap_name);
        emit_string(o, "mqtt_host", full->network.mqtt_host); emit_int(o, "mqtt_port", full->network.mqtt_port); emit_string(o, "mqtt_user", full->network.mqtt_user);
        emit_string(o, "mqtt_topic_mode", "tmcp");
        emit_string(o, "mqtt_protocol", "tmcp"); emit_int(o, "mqtt_protocol_version", 2);

        emit_int(o, "timezone_minutes", full->model.m1.timezone_minutes);
        const char *ntp_host = full->model.m1.ntp_host;
        emit_string(o, "ntp_host", ntp_host);
        emit_string(o, "ntp_status", full->model.m1.ntp_status[0] ? full->model.m1.ntp_status : "waiting_network");
        emit_string(o, "ntp_server", full->model.m1.ntp_server);
        emit_bool(o, "ntp_synced", full->model.m1.ntp_synced);
        emit_bool(o, "ntp_screen_queued", full->model.m1.ntp_screen_queued);
        /* This SDK serializes doubles with %g (six significant digits), which
         * rounds Unix timestamps by minutes. Preserve whole seconds as an integer. */
        if (full->model.m1.ntp_synced) emit_int(o,"ntp_last_success",full->model.m1.ntp_last_success);
        else emit_null(o,"ntp_last_success");
        emit_int(o, "ntp_next_seconds", snapshot->network.wifi_connected && full->model.m1.ntp_next_at &&
            (int32_t)(full->model.m1.ntp_next_at - now) > 0 ? (uint32_t)(full->model.m1.ntp_next_at - now) / 1000 : 0);
        emit_bool(o, "mqtt_password_set", full->network.mqtt_password_set);
        emit_bool(o, "web_password_set", full->network.web_password_set);
    }
    emit_string(o, "mqtt_status", device_mqtt_status(snapshot->network.mqtt_status));
    emit_bool(o, "mqtt_cleanup_warning", snapshot->network.mqtt_cleanup_warning);
    emit_bool(o, "wifi_connected", snapshot->network.wifi_connected); emit_bool(o, "mqtt_connected", snapshot->network.mqtt_connected);
    emit_bool(o, "ap_active", snapshot->network.ap_active); emit_bool(o, "uart_ready", snapshot->sensor.m1.uart_ready);
    emit_bool(o, "sensor_valid", fresh); emit_bool(o, "sensor_received", snapshot->sensor.m1.sample.received);
    if (snapshot->sensor.m1.sample.received) emit_double(o,"age_ms",(uint32_t)(now - snapshot->sensor.m1.sample.updated_ms));
    else emit_null(o,"age_ms");
    emit_begin(o,"sensor_health",false);
    const m1_sample_t *sample=&snapshot->sensor.m1.sample;
    emit_bool(o,"received",sample->received);

    emit_bool(o,"pm25_valid",fresh && sample->pm25_status);emit_bool(o,"climate_valid",fresh && sample->climate_status);
    emit_bool(o,"all_flags_ok",fresh && sample->pm25_status && sample->climate_status && sample->auxiliary_status);
    emit_end(o);
    emit_begin(o,"co2_display",false);
    const device_m1_co2_t *c=&snapshot->sensor.m1.co2_display;
    emit_bool(o,"available",c->supported && c->present);
    emit_bool(o,"mcu_supported",c->supported);emit_bool(o,"module_present",c->present);
    emit_int(o,"revision",c->revision);emit_int(o,"mode",c->mode);emit_int(o,"slot",c->slot);
    emit_int(o,"interval_seconds",c->interval);emit_int(o,"low",c->low);emit_int(o,"high",c->high);
    emit_int(o,"mcu_co2_ppm",c->ppm);emit_bool(o,"mcu_data_valid",c->valid);emit_bool(o,"showing_co2",c->showing);
    emit_end(o);
    emit_begin(o,"scd40",false);
    bool co2_fresh = snapshot->sensor.m1.co2.ready && snapshot->sensor.m1.co2.received &&
        (uint32_t)(now - snapshot->sensor.m1.co2.updated_ms) < 20000;
    emit_bool(o, "ready", snapshot->sensor.m1.co2.ready); emit_bool(o, "valid", co2_fresh);

    if (snapshot->sensor.m1.co2.received) emit_int(o, "age_ms", (uint32_t)(now - snapshot->sensor.m1.co2.updated_ms));
    else emit_null(o,"age_ms");
    if (co2_fresh) {
        emit_int(o, "co2_ppm", snapshot->sensor.m1.co2.co2_ppm);
    } else {
        emit_null(o,"co2_ppm");
    }
    emit_end(o);
    emit_begin(o,"barometer",false);
    const barometer_state_t *b = &snapshot->sensor.m1.barometer;
    const char *barometer_model = m1_barometer_model(b->chip_id);
    bool pressure_fresh = m1_barometer_fresh(b, now);
    emit_bool(o, "ready", b->ready);
    emit_bool(o, "valid", pressure_fresh);
    if (barometer_model) {
        emit_string(o, "model", barometer_model);

    } else {
        emit_null(o,"model");

    }
    if (pressure_fresh)
        emit_double(o,"pressure_hpa",b->pressure_centi_pa / 10000.0);
    else emit_null(o,"pressure_hpa");
    if (b->received) emit_int(o, "age_ms", (uint32_t)(now - b->updated_ms));
    else emit_null(o,"age_ms");

    emit_end(o);
    emit_int(o, "brightness_requested", snapshot->control.m1.brightness_requested);
    if (snapshot->sensor.m1.sample.received) emit_double(o,"temperature",snapshot->sensor.m1.sample.temperature_deci_c / 10.0);
    else emit_null(o,"temperature");
    if (snapshot->sensor.m1.sample.received) emit_double(o,"humidity",snapshot->sensor.m1.sample.humidity_deci_pct / 10.0);
    else emit_null(o,"humidity");
    if (snapshot->sensor.m1.sample.received) emit_double(o,"formaldehyde",snapshot->sensor.m1.sample.hcho_ug_m3 / 1000.0);
    else emit_null(o,"formaldehyde");
    if (snapshot->sensor.m1.sample.received) emit_int(o,"pm25",snapshot->sensor.m1.sample.pm25_ug_m3);
    else emit_null(o,"pm25");

    emit_string(o, "ota_state", snapshot->ota.state); emit_string(o, "ota_error", snapshot->ota.error);
    emit_int(o, "ota_progress", snapshot->ota.progress); emit_bool(o, "ota_active", snapshot->ota.active);
    emit_bool(o, "scan_busy", snapshot->ota.scan_busy);
}

#endif
#endif
#if !defined(PHICOMM_STATUS_MODEL) || PHICOMM_STATUS_MODEL == 2
static __attribute__((noinline)) void encode_tc1(status_sink *o,const device_status_t *s,const device_status_full_t *full)
{
    if(full){
        emit_int(o,"utc_time",full->model.tc1.utc_time);emit_bool(o,"time_synced",full->model.tc1.time_synced);
    }
    emit_string(o,"wifi_trial_status",s->network.wifi_trial_status);
    emit_string(o,"version",s->identity.version);emit_string(o,"device_id",s->identity.id);emit_string(o,"name",s->identity.name);
    if(full){
        emit_string(o,"ssid",full->network.ssid);emit_string(o,"ip",full->network.ip);
        emit_string(o,"mask",full->model.tc1.mask);emit_string(o,"gateway",full->model.tc1.gateway);
        emit_string(o,"ap_name",full->network.ap_name);emit_string(o,"mqtt_host",full->network.mqtt_host);emit_string(o,"mqtt_user",full->network.mqtt_user);
        emit_string(o,"mqtt_topic_mode",full->model.tc1.modern?"tmcp":"legacy");
        emit_string(o,"mqtt_protocol",full->model.tc1.modern?"tmcp":"legacy");emit_int(o,"mqtt_protocol_version",full->model.tc1.modern?2:1);
        emit_begin(o,"socket_names",true);
        for(unsigned i=0;i<6;i++)emit_string(o,NULL,full->model.tc1.socket_names[i]);
        emit_end(o);
        emit_int(o,"mqtt_port",full->network.mqtt_port);
        emit_int(o,"report_seconds",full->model.tc1.report_seconds);emit_int(o,"powerCoefficientX100",full->model.tc1.power_coefficient);

        emit_bool(o,"wifi_password_set",full->network.wifi_password_set);emit_bool(o,"mqtt_password_set",full->network.mqtt_password_set);emit_bool(o,"web_password_set",full->network.web_password_set);
        variant_fields(o);
    }
    emit_string(o,"model","tc1");emit_int(o,"uptime_seconds",s->now_ms/1000);
    emit_begin(o,"socket_states",true);
    for(unsigned i=0;i<6;i++)emit_int(o,NULL,s->control.tc1.states[i]);
    emit_end(o);
    emit_bool(o,"led_enabled",s->control.tc1.led_enabled);emit_bool(o,"child_lock",s->control.tc1.child_lock);
    emit_string(o,"mqtt_status",device_mqtt_status(s->network.mqtt_status));
    emit_bool(o,"wifi_connected",s->network.wifi_connected);emit_bool(o,"ap_active",s->network.ap_active);
    emit_bool(o,"mqtt_cleanup_warning",s->network.mqtt_cleanup_warning);emit_bool(o,"mqtt_connected",s->network.mqtt_connected);
    emit_string(o,"ota_state",s->ota.state);emit_string(o,"ota_error",s->ota.error);emit_int(o,"ota_progress",s->ota.progress);
}

#endif
static bool device_status_visit(status_sink *o,const device_status_t *s,const device_status_full_t *full,const char *key)
{
    if(!s || (s->captured&(STATUS_IDENTITY|STATUS_NETWORK|STATUS_OTA))!=(STATUS_IDENTITY|STATUS_NETWORK|STATUS_OTA))return false;
    if(full && !(s->captured&STATUS_DIAGNOSTIC))return false;
    bool valid=false;
    emit_begin(o,key,false);
#if defined(PHICOMM_APPLICATION_WATCHDOG)
#if !defined(PHICOMM_STATUS_MODEL) || PHICOMM_STATUS_MODEL == 0
    if(s->model==DEVICE_A1 && (s->captured&STATUS_CONTROL)){encode_a1(o,s,full);valid=true;}
#endif
#if !defined(PHICOMM_STATUS_MODEL) || PHICOMM_STATUS_MODEL == 1
    if(s->model==DEVICE_M1 && (s->captured&STATUS_SENSOR)){encode_m1(o,s,full);valid=true;}
#endif
#endif
#if !defined(PHICOMM_STATUS_MODEL) || PHICOMM_STATUS_MODEL == 2
    if(s->model==DEVICE_TC1 && (s->captured&STATUS_CONTROL)){encode_tc1(o,s,full);valid=true;}
#endif
#ifdef PHICOMM_BEMFA
    if(valid && s->model!=DEVICE_M1) {
        emit_bool(o,"mqtt_enabled",s->network.mqtt_enabled);
        emit_bool(o,"bemfa_enabled",s->network.bemfa_enabled);
        emit_bool(o,"bemfa_connected",s->network.bemfa_status==5);
        emit_string(o,"bemfa_status",device_mqtt_status(s->network.bemfa_status));

    }
#endif
    emit_end(o);
    return valid && !o->failed;
}

/* Emit members into a caller-opened runtime/capability map. */
void device_runtime_mp(ph_writer *o,const device_runtime_t *s)
{
    if(!o)return;
    if(!s){o->failed=true;return;}
    runtime_fields(o,s);
}
/* Complete frozen payload only. Transport envelope and caching belong to caller. */
void device_status_mp(ph_writer *o,const device_status_t *s,const device_status_full_t *full)
{
    if(!o)return;
    /* Only a cold standalone status needs a sizing walk. Both walks use the
     * same frozen snapshot; no recapture, mutation or temporary payload exists.
     * Warm caches already own their capacity and take the single-pass path. */
    if(!o->fixed && !o->depth && o->wire && !o->wire->len &&
       o->wire->cap==PHW_INITIAL_CAPACITY){
        ph_writer measure;ph_buf measured;
        if(!phw_init_measure(&measure,PH_WIRE_MSGPACK,&measured) || !full ||
           !device_status_visit(&measure,s,full,NULL) ||
           !phw_finish_fixed(&measure) ||
           !ph_buf_reserve(o->wire,measured.len+1)){
            o->failed=true;return;
        }
    }
    if(!full||!device_status_visit(o,s,full,NULL))o->failed=true;
}

/* On-demand diagnostics. Stable IDs are generated from diagnostics-schema.json.
 * Pairs keep numeric IDs without relaxing the string-map request parser. */
#include "diagnostics_ids.h"
static void diagnostic_field(ph_writer *w,unsigned id){phw_begin(w,NULL,true);phw_u32(w,NULL,id);}
void device_diagnostics_mp(ph_writer *w,const device_status_t *s,const device_status_full_t *f)
{
 if(!w)return;
 if(!s||!f||!(s->captured&STATUS_DIAGNOSTIC)){w->failed=true;return;}
 /* Product builds already fix this model for the business status visitor.
  * Apply the same selection to diagnostics: omit unreachable field emitters
  * and do not format another product's union member on every request. */
#if !defined(PHICOMM_STATUS_MODEL) || PHICOMM_STATUS_MODEL != 2
 static const char hex[]="0123456789abcdef";
#endif
#if !defined(PHICOMM_STATUS_MODEL) || PHICOMM_STATUS_MODEL == 0
 char raw[7];
 for(unsigned i=0;i<3;i++){unsigned b=f->model.a1.panel_raw[i];raw[i*2]=hex[b>>4];raw[i*2+1]=hex[b&15];}raw[6]=0;
#endif
#if !defined(PHICOMM_STATUS_MODEL) || PHICOMM_STATUS_MODEL == 1
 char frame[41];
 for(unsigned i=0;i<20;i++){unsigned b=f->model.m1.last_frame[i];frame[i*2]=hex[b>>4];frame[i*2+1]=hex[b&15];}frame[40]=0;
#endif
 phw_begin(w,NULL,false);phw_u32(w,"schema",PH_DIAGNOSTICS_SCHEMA);
 phw_begin(w,"fields",true);
 if(f->runtime.reset_valid){diagnostic_field(w,PH_DIAG_LAST_RESET_REASON);phw_i32(w,NULL,f->runtime.reset_reason);phw_end(w);}
 {diagnostic_field(w,PH_DIAG_UPTIME_MS);phw_u32(w,NULL,s->now_ms);phw_end(w);}
 {diagnostic_field(w,PH_DIAG_UPTIME_SECONDS);phw_i32(w,NULL,s->now_ms/1000);phw_end(w);}
 if(f->runtime.memory_valid){diagnostic_field(w,PH_DIAG_FREE_MEMORY);phw_i32(w,NULL,f->runtime.free_memory);phw_end(w);}
 if(f->runtime.memory_valid){diagnostic_field(w,PH_DIAG_ALLOCATED_MEMORY);phw_i32(w,NULL,f->runtime.allocated_memory);phw_end(w);}
 if(f->runtime.memory_valid){diagnostic_field(w,PH_DIAG_FREE_MEMORY_CHUNKS);phw_i32(w,NULL,f->runtime.free_memory_chunks);phw_end(w);}
 if(f->runtime.memory_valid){diagnostic_field(w,PH_DIAG_MIN_OBSERVED_FREE_MEMORY);phw_i32(w,NULL,f->runtime.minimum_free);phw_end(w);}
#if !defined(PHICOMM_STATUS_MODEL) || PHICOMM_STATUS_MODEL != 2
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_RESET_REASON);phw_text(w,NULL,f->watchdog.reset_name);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_RESET_REASON_RAW);phw_i32(w,NULL,f->watchdog.reset_raw);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_WATCHDOG_ENABLED);phw_bool(w,NULL,f->watchdog.enabled);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_WATCHDOG_STATE);phw_text(w,NULL,f->watchdog.failed?"fault":f->watchdog.ready?"healthy":"starting");phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_WATCHDOG_FAULT);phw_text(w,NULL,f->watchdog.fault);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_OK);phw_i32(w,NULL,(int)f->http.accepted);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_FAIL);phw_i32(w,NULL,(int)f->http.accept_failures);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_ERRNO);phw_i32(w,NULL,f->http.accept_errno);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_LAST_FD);phw_i32(w,NULL,f->http.last_fd);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_QUEUE_OK);phw_i32(w,NULL,(int)f->http.queued);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_QUEUE_REJECT);phw_i32(w,NULL,(int)f->http.rejected);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_BUSY_FCNTL_FAIL);phw_i32(w,NULL,(int)f->http.nonblock_failures);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_BUSY_DRAINED);phw_i32(w,NULL,(int)f->http.drained);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_BUSY_SEND_OK);phw_i32(w,NULL,(int)f->http.busy_sent);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_BUSY_SEND_FAIL);phw_i32(w,NULL,(int)f->http.busy_failed);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_BUSY_SEND_ERRNO);phw_i32(w,NULL,f->http.busy_errno);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_BUSY_SEND_BYTES);phw_i32(w,NULL,(int)f->http.busy_bytes);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_OVERFLOW_CLOSE);phw_i32(w,NULL,(int)f->http.rejected);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_QUEUE_CAPACITY);phw_i32(w,NULL,PH_HTTP_WAIT_SLOTS);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_HTTP_TCP_LIMIT_REQUESTED);phw_i32(w,NULL,PH_HTTP_WAIT_SLOTS+1);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_HTTP_TCP_LIMIT_SET_RC);phw_i32(w,NULL,f->http.set_rc);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_HTTP_TCP_LIMIT_SET_ERRNO);phw_i32(w,NULL,f->http.set_errno);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_HTTP_TCP_LIMIT_GET_RC);phw_i32(w,NULL,f->http.limit_get_rc);phw_end(w);}
 if(s->model!=DEVICE_TC1&&f->http.limit_get_rc==0){diagnostic_field(w,PH_DIAG_HTTP_TCP_LIMIT_READBACK);phw_i32(w,NULL,f->http.limit);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_HTTP_TCP_CONNECTIONS_GET_RC);phw_i32(w,NULL,f->http.connections_get_rc);phw_end(w);}
 if(s->model!=DEVICE_TC1&&f->http.connections_get_rc==0){diagnostic_field(w,PH_DIAG_HTTP_TCP_CONNECTIONS);phw_i32(w,NULL,f->http.connections);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_MQTT_ERRORS);phw_i32(w,NULL,f->network.mqtt_errors);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_MQTT_LAST_ERROR);phw_text(w,NULL,f->network.mqtt_last_error);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_OTA_SIGNATURE_VERIFIED);phw_bool(w,NULL,f->ota.signature_verified);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_OTA_IMAGE_VERIFIED);phw_bool(w,NULL,f->ota.image_verified);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_OTA_KEY_ID);phw_i32(w,NULL,f->ota.key_id);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_OTA_IMAGE_LENGTH);phw_i32(w,NULL,f->ota.image_length);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_OTA_RELEASE_COUNTER);phw_text(w,NULL,f->ota.release_counter);phw_end(w);}
 if(s->model!=DEVICE_TC1){diagnostic_field(w,PH_DIAG_OTA_VERSION);phw_text(w,NULL,f->ota.version);phw_end(w);}
#endif
#if !defined(PHICOMM_STATUS_MODEL) || PHICOMM_STATUS_MODEL == 0
 if(s->model==DEVICE_A1){diagnostic_field(w,PH_DIAG_PANEL_ERRORS);phw_i32(w,NULL,f->model.a1.panel_errors);phw_end(w);}
 if(s->model==DEVICE_A1){diagnostic_field(w,PH_DIAG_HARDWARE_ERRORS);phw_i32(w,NULL,f->model.a1.hardware_errors);phw_end(w);}
 if(s->model==DEVICE_A1){diagnostic_field(w,PH_DIAG_PANEL_LAST_ERROR);phw_i32(w,NULL,f->model.a1.panel_last_error);phw_end(w);}
 if(s->model==DEVICE_A1){diagnostic_field(w,PH_DIAG_PANEL_RAW);phw_text(w,NULL,raw);phw_end(w);}
#endif
#if !defined(PHICOMM_STATUS_MODEL) || PHICOMM_STATUS_MODEL == 1
 if(s->model==DEVICE_M1){diagnostic_field(w,PH_DIAG_RX_FRAMES);phw_i32(w,NULL,f->model.m1.rx_frames);phw_end(w);}
 if(s->model==DEVICE_M1){diagnostic_field(w,PH_DIAG_UNKNOWN_FRAMES);phw_i32(w,NULL,f->model.m1.unknown_frames);phw_end(w);}
 if(s->model==DEVICE_M1){diagnostic_field(w,PH_DIAG_UART_ERRORS);phw_i32(w,NULL,f->model.m1.uart_errors);phw_end(w);}
 if(s->model==DEVICE_M1){diagnostic_field(w,PH_DIAG_DISCARDED_BYTES);phw_i32(w,NULL,f->model.m1.discarded_bytes);phw_end(w);}
 if(s->model==DEVICE_M1){diagnostic_field(w,PH_DIAG_LAST_FRAME);phw_text(w,NULL,frame);phw_end(w);}
 if(s->model==DEVICE_M1){diagnostic_field(w,PH_DIAG_SENSOR_HEALTH_PM25_RAW);phw_i32(w,NULL,s->sensor.m1.sample.pm25_status);phw_end(w);}
 if(s->model==DEVICE_M1){diagnostic_field(w,PH_DIAG_SENSOR_HEALTH_CLIMATE_RAW);phw_i32(w,NULL,s->sensor.m1.sample.climate_status);phw_end(w);}
 if(s->model==DEVICE_M1){diagnostic_field(w,PH_DIAG_SENSOR_HEALTH_AUXILIARY_RAW);phw_i32(w,NULL,s->sensor.m1.sample.auxiliary_status);phw_end(w);}
 if(s->model==DEVICE_M1){diagnostic_field(w,PH_DIAG_SCD40_SERIAL);phw_text(w,NULL,s->sensor.m1.co2.serial);phw_end(w);}
 if(s->model==DEVICE_M1){diagnostic_field(w,PH_DIAG_SCD40_ERRORS);phw_i32(w,NULL,s->sensor.m1.co2.errors);phw_end(w);}
 if(s->model==DEVICE_M1){diagnostic_field(w,PH_DIAG_SCD40_LAST_ERROR);phw_i32(w,NULL,s->sensor.m1.co2.last_error);phw_end(w);}
 if(s->model==DEVICE_M1){diagnostic_field(w,PH_DIAG_SCD40_SAMPLES);phw_i32(w,NULL,f->model.m1.co2_samples);phw_end(w);}
 if(s->model==DEVICE_M1){diagnostic_field(w,PH_DIAG_SCD40_CRC_ERRORS);phw_i32(w,NULL,f->model.m1.co2_crc_errors);phw_end(w);}
 if(s->model==DEVICE_M1){diagnostic_field(w,PH_DIAG_SCD40_BUS_INITIALIZED);phw_bool(w,NULL,f->model.m1.co2_bus_initialized);phw_end(w);}
 if(s->model==DEVICE_M1){diagnostic_field(w,PH_DIAG_SCD40_LAST_COMMAND);phw_i32(w,NULL,f->model.m1.co2_last_command);phw_end(w);}
 if(s->model==DEVICE_M1){diagnostic_field(w,PH_DIAG_SCD40_ERROR_PHASE);phw_i32(w,NULL,f->model.m1.co2_error_phase);phw_end(w);}
 if(s->model==DEVICE_M1&&m1_barometer_model(s->sensor.m1.barometer.chip_id)){diagnostic_field(w,PH_DIAG_BAROMETER_CHIP_ID);phw_i32(w,NULL,s->sensor.m1.barometer.chip_id);phw_end(w);}
 if(s->model==DEVICE_M1&&m1_barometer_model(s->sensor.m1.barometer.chip_id)){diagnostic_field(w,PH_DIAG_BAROMETER_ADDRESS);phw_i32(w,NULL,s->sensor.m1.barometer.address);phw_end(w);}
 if(s->model==DEVICE_M1){diagnostic_field(w,PH_DIAG_BAROMETER_SAMPLES);phw_i32(w,NULL,s->sensor.m1.barometer.samples);phw_end(w);}
 if(s->model==DEVICE_M1){diagnostic_field(w,PH_DIAG_BAROMETER_ERRORS);phw_i32(w,NULL,s->sensor.m1.barometer.errors);phw_end(w);}
 if(s->model==DEVICE_M1){diagnostic_field(w,PH_DIAG_BAROMETER_ATTEMPTS);phw_i32(w,NULL,s->sensor.m1.barometer.attempts);phw_end(w);}
 if(s->model==DEVICE_M1){diagnostic_field(w,PH_DIAG_BAROMETER_LAST_ERROR);phw_i32(w,NULL,s->sensor.m1.barometer.last_error);phw_end(w);}
#endif
#if defined(PHICOMM_BEMFA) && (!defined(PHICOMM_STATUS_MODEL) || PHICOMM_STATUS_MODEL != 1)
 if(s->model!=DEVICE_M1){diagnostic_field(w,PH_DIAG_BEMFA_ERRORS);phw_i32(w,NULL,s->network.bemfa_errors);phw_end(w);}
 if(s->model!=DEVICE_M1){diagnostic_field(w,PH_DIAG_BEMFA_LAST_STAGE);phw_i32(w,NULL,s->network.bemfa_last_stage);phw_end(w);}
 if(s->model!=DEVICE_M1){diagnostic_field(w,PH_DIAG_BEMFA_LAST_ERROR);phw_i32(w,NULL,s->network.bemfa_last_error);phw_end(w);}
#endif
 if(f->service_memory_valid){
  diagnostic_field(w,PH_DIAG_SERVICE_MEMORY_RESERVED);phw_u32(w,NULL,f->service_memory.reserved);phw_end(w);
  diagnostic_field(w,PH_DIAG_SERVICE_MEMORY_ACTIVE);phw_u32(w,NULL,f->service_memory.active);phw_end(w);
  diagnostic_field(w,PH_DIAG_SERVICE_MEMORY_DENIED);phw_u32(w,NULL,f->service_memory.denied);phw_end(w);
  diagnostic_field(w,PH_DIAG_SERVICE_MEMORY_WAITING);phw_u32(w,NULL,f->service_memory.waiting);phw_end(w);
  diagnostic_field(w,PH_DIAG_SERVICE_MEMORY_MINIMUM_FREE);phw_u32(w,NULL,f->service_memory.minimum_free);phw_end(w);
  diagnostic_field(w,PH_DIAG_SERVICE_MEMORY_MQTT_PENDING);phw_u32(w,NULL,f->service_memory.mqtt_pending);phw_end(w);
  diagnostic_field(w,PH_DIAG_SERVICE_MEMORY_MQTT_PEAK);phw_u32(w,NULL,f->service_memory.mqtt_peak);phw_end(w);
  diagnostic_field(w,PH_DIAG_SERVICE_MEMORY_MQTT_OVERFLOW);phw_u32(w,NULL,f->service_memory.mqtt_overflow);phw_end(w);
  diagnostic_field(w,PH_DIAG_SERVICE_MEMORY_MQTT_MAX_DELAY);phw_u32(w,NULL,f->service_memory.mqtt_max_delay);phw_end(w);
  diagnostic_field(w,PH_DIAG_SERVICE_MEMORY_WIRE_ALLOC_FAILED);phw_u32(w,NULL,f->service_memory.wire_alloc_failed);phw_end(w);
  diagnostic_field(w,PH_DIAG_SERVICE_MEMORY_WIRE_GUARD_DENIED);phw_u32(w,NULL,f->service_memory.wire_guard_denied);phw_end(w);
 }
 phw_end(w);phw_end(w);
}

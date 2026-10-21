/* Request-local frozen status encoder shared by HTTP and MQTT. */
#include "device_status.h"
#include "json_build.h"
#include "firmware_variant.h"
#include <stdio.h>
static void status_string(struct json_object *o,const char *k,const char *s)
{json_build_object_add(o,k,json_object_new_string(s));}
static void status_int(struct json_object *o,const char *k,int n)
{json_build_object_add(o,k,json_object_new_int(n));}
static void status_bool(struct json_object *o,const char *k,bool b)
{json_build_object_add(o,k,json_object_new_boolean(b));}
static const char *device_mqtt_status(unsigned state)
{
    static const char *const names[]={"disconnected","connecting","config_error","auth_failed","waiting_acl","ready"};
    return state<sizeof(names)/sizeof(*names)?names[state]:names[0];
}
void device_runtime_encode(struct json_object *o,const device_runtime_t *s)
{
    if(s->reset_valid)status_int(o,"last_reset_reason",s->reset_reason);
    json_build_object_add(o,"uptime_ms",json_object_new_double(s->now_ms));
    status_int(o,"uptime_seconds",s->now_ms/1000);
    if(!s->memory_valid)return;
    status_int(o,"free_memory",s->free_memory);status_int(o,"allocated_memory",s->allocated_memory);
    status_int(o,"free_memory_chunks",s->free_memory_chunks);status_int(o,"min_observed_free_memory",s->minimum_free);
}
#if defined(PHICOMM_APPLICATION_WATCHDOG)
static void device_http_encode(json_object *o,const ph_http_diagnostics *snapshot,unsigned requested)
{
    ph_http_diagnostics diag = *snapshot;
    status_int(o, "http_accept_ok", (int)diag.accepted);
    status_int(o, "http_accept_fail", (int)diag.accept_failures);
    status_int(o, "http_accept_errno", diag.accept_errno);
    status_int(o, "http_accept_last_fd", diag.last_fd);
    status_int(o, "http_accept_queue_ok", (int)diag.queued);
    status_int(o, "http_accept_queue_reject", (int)diag.rejected);
    json_object_object_add(o,"http_accept_queue_last_rc",NULL);
    status_int(o, "http_accept_busy_fcntl_fail", (int)diag.nonblock_failures);
    status_int(o, "http_accept_busy_drained", (int)diag.drained);
    status_int(o, "http_accept_busy_send_ok", (int)diag.busy_sent);
    status_int(o, "http_accept_busy_send_fail", (int)diag.busy_failed);
    status_int(o, "http_accept_busy_send_errno", diag.busy_errno);
    status_int(o, "http_accept_busy_send_bytes", (int)diag.busy_bytes);
    status_int(o, "http_accept_overflow_close", (int)diag.rejected);
    status_int(o, "http_accept_queue_capacity", 1);
    status_int(o, "http_tcp_limit_requested", requested);
    status_int(o, "http_tcp_limit_set_rc", diag.set_rc);
    status_int(o, "http_tcp_limit_set_errno", diag.set_errno);
    status_int(o, "http_tcp_limit_get_rc", diag.limit_get_rc);
    if (diag.limit_get_rc == 0) json_build_object_add(o, "http_tcp_limit_readback",json_object_new_int(diag.limit));
    else json_object_object_add(o, "http_tcp_limit_readback", NULL);
    status_int(o, "http_tcp_connections_get_rc", diag.connections_get_rc);
    if (diag.connections_get_rc == 0) json_build_object_add(o, "http_tcp_connections",json_object_new_int(diag.connections));
    else json_object_object_add(o, "http_tcp_connections", NULL);
}
static json_object *device_filter_encode(const a1_filter_storage_t *f)
{
    json_object *o=json_object_new_object();unsigned remaining=a1_filter_remaining(f);
    status_int(o,"used_seconds",f->used_seconds);status_int(o,"lifetime_hours",f->lifetime_hours);
    status_int(o,"remaining_percent",remaining);status_bool(o,"replacement_due",remaining<=10);
    return o;
}
static __attribute__((noinline)) json_object *encode_a1(const device_status_t *snapshot, const device_status_full_t *full)
{
    bool live = !full;

    json_object *o = json_object_new_object();
    if (!o) return NULL;
    status_string(o,"model","a1");
    if (!live) {
        phicomm_variant_json(o);
        device_runtime_encode(o, &full->runtime);
        device_http_encode(o,&full->http,3);
        watchdog_status_encode(o, &full->watchdog);
        json_build_object_add(o, "filter", device_filter_encode(&full->model.a1.filter));
    } else status_int(o, "uptime_seconds", snapshot->now_ms/1000);
    status_string(o, "version", snapshot->identity.version); status_string(o, "device_id", snapshot->identity.id); status_string(o, "name", snapshot->identity.name);
    status_bool(o, "wifi_trial_active", snapshot->network.wifi_trial_active);
    status_string(o, "wifi_trial_status", snapshot->network.wifi_trial_status);
    if (!live) {
        status_string(o, "ssid", full->network.ssid); status_string(o, "ip", full->network.ip); status_string(o, "ap_name", full->network.ap_name);
        status_string(o, "mqtt_host", full->network.mqtt_host); status_int(o, "mqtt_port", full->network.mqtt_port); status_string(o, "mqtt_user", full->network.mqtt_user);
        status_string(o, "mqtt_topic_mode", "tmcp");
        status_string(o, "mqtt_protocol", "tmcp"); status_int(o, "mqtt_protocol_version", 2);
        status_int(o, "mqtt_errors", full->network.mqtt_errors); status_string(o, "mqtt_last_error", full->network.mqtt_last_error);
        status_bool(o, "web_password_set", full->network.web_password_set);
        status_bool(o, "wifi_password_set", full->network.wifi_password_set);
        status_bool(o, "mqtt_password_set", full->network.mqtt_password_set);
    }
    status_string(o, "mqtt_status", device_mqtt_status(snapshot->network.mqtt_status));
    status_bool(o, "mqtt_cleanup_warning", snapshot->network.mqtt_cleanup_warning);
    status_bool(o, "wifi_connected", snapshot->network.wifi_connected); status_bool(o, "mqtt_connected", snapshot->network.mqtt_connected);
    bool effective_on=snapshot->control.a1.effective_on; unsigned effective_speed=snapshot->control.a1.effective_speed;
    status_string(o,"fan_mode",snapshot->control.a1.automatic?"auto":"manual");
    status_string(o,"fan_state",!effective_on?"off":effective_speed?"running":"idle");
    status_int(o,"auto_max_speed",snapshot->control.a1.auto_limit);
    status_bool(o,"requested_fan_on",snapshot->control.a1.fan_on); status_int(o,"requested_speed",snapshot->control.a1.speed);
    status_bool(o, "ap_active", snapshot->network.ap_active); status_bool(o, "fan_on", effective_on);
    status_int(o, "speed", snapshot->control.a1.speed); status_int(o, "percentage", effective_on ? effective_speed : 0);
    status_int(o, "applied_speed", snapshot->control.a1.applied_speed); status_bool(o, "fan_ready", snapshot->control.a1.fan_ready);
    status_bool(o, "touch_active", snapshot->control.a1.touch_active);
    status_bool(o, "led_enabled", snapshot->control.a1.led_enabled); status_bool(o, "led_visible", snapshot->control.a1.led_visible);
    status_bool(o, "led_always_on", snapshot->control.a1.led_always_on); status_int(o, "brightness", snapshot->control.a1.brightness);
    status_int(o, "red", snapshot->control.a1.red); status_int(o, "green", snapshot->control.a1.green); status_int(o, "blue", snapshot->control.a1.blue);
    status_bool(o, "child_lock", snapshot->control.a1.child_lock); status_bool(o, "panel_ready", snapshot->control.a1.panel_ready);
    if (!live) {
        status_int(o, "panel_errors", full->model.a1.panel_errors); status_int(o, "hardware_errors", full->model.a1.hardware_errors);
        status_int(o, "panel_last_error", full->model.a1.panel_last_error);
        char raw[7];
        for (unsigned i = 0; i < 3; i++) snprintf(raw + 2 * i, 3, "%02x", full->model.a1.panel_raw[i]);
        status_string(o, "panel_raw", raw);
    }
    status_bool(o, "config_dirty", snapshot->control.a1.config_dirty); status_string(o, "save_error", snapshot->control.a1.save_error);
    if (!live) {
        status_bool(o,"ota_signature_verified",full->ota.signature_verified);
        status_bool(o,"ota_image_verified",full->ota.image_verified);
        status_int(o,"ota_key_id",full->ota.key_id);status_int(o,"ota_image_length",full->ota.image_length);
        status_string(o,"ota_release_counter",full->ota.release_counter);status_string(o,"ota_version",full->ota.version);
    }
    status_string(o, "ota_state", snapshot->ota.state); status_string(o, "ota_error", snapshot->ota.error);
    status_int(o, "ota_progress", snapshot->ota.progress); status_bool(o, "ota_active", snapshot->ota.active);
    status_bool(o, "reboot_pending", snapshot->ota.reboot_pending); status_bool(o, "scan_busy", snapshot->ota.scan_busy);
    return o;
}

static __attribute__((noinline)) json_object *encode_m1(const device_status_t *snapshot, const device_status_full_t *full)
{
    bool live = !full;

    uint32_t now = snapshot->now_ms; bool fresh = snapshot->sensor.m1.sample.received && (uint32_t)(now-snapshot->sensor.m1.sample.updated_ms)<M1_STALE_MS;
    json_object *o = json_object_new_object();
    if (!o) return NULL;
    status_string(o,"model","m1");
    if (!live) {
        phicomm_variant_json(o);
        device_runtime_encode(o, &full->runtime);
        device_http_encode(o,&full->http,4);
        watchdog_status_encode(o, &full->watchdog);
    } else status_int(o, "uptime_seconds", snapshot->now_ms/1000);
    status_string(o, "version", snapshot->identity.version); status_string(o, "device_id", snapshot->identity.id); status_string(o, "name", snapshot->identity.name);
    status_bool(o, "wifi_trial_active", snapshot->network.wifi_trial_active);
    status_string(o, "wifi_trial_status", snapshot->network.wifi_trial_status);
    if (!live) {
        status_string(o, "ssid", full->network.ssid); status_string(o, "ip", full->network.ip); status_string(o, "ap_name", full->network.ap_name);
        status_string(o, "mqtt_host", full->network.mqtt_host); status_int(o, "mqtt_port", full->network.mqtt_port); status_string(o, "mqtt_user", full->network.mqtt_user);
        status_string(o, "mqtt_topic_mode", "tmcp");
        status_string(o, "mqtt_protocol", "tmcp"); status_int(o, "mqtt_protocol_version", 2);
        status_int(o, "mqtt_errors", full->network.mqtt_errors); status_string(o, "mqtt_last_error", full->network.mqtt_last_error);
        status_int(o, "timezone_minutes", full->model.m1.timezone_minutes);
        const char *ntp_host = full->model.m1.ntp_host;
        status_string(o, "ntp_host", ntp_host);
        status_string(o, "ntp_status", full->model.m1.ntp_status[0] ? full->model.m1.ntp_status : "waiting_network");
        status_string(o, "ntp_server", full->model.m1.ntp_server);
        status_bool(o, "ntp_synced", full->model.m1.ntp_synced);
        status_bool(o, "ntp_screen_queued", full->model.m1.ntp_screen_queued);
        /* This SDK serializes doubles with %g (six significant digits), which
         * rounds Unix timestamps by minutes. Preserve whole seconds as an integer. */
        if (full->model.m1.ntp_synced) json_build_object_add(o, "ntp_last_success",json_object_new_int(full->model.m1.ntp_last_success));
        else json_object_object_add(o, "ntp_last_success", NULL);
        status_int(o, "ntp_next_seconds", snapshot->network.wifi_connected && full->model.m1.ntp_next_at &&
            (int32_t)(full->model.m1.ntp_next_at - now) > 0 ? (uint32_t)(full->model.m1.ntp_next_at - now) / 1000 : 0);
        status_bool(o, "mqtt_password_set", full->network.mqtt_password_set);
        status_bool(o, "web_password_set", full->network.web_password_set);
    }
    status_string(o, "mqtt_status", device_mqtt_status(snapshot->network.mqtt_status));
    status_bool(o, "mqtt_cleanup_warning", snapshot->network.mqtt_cleanup_warning);
    status_bool(o, "wifi_connected", snapshot->network.wifi_connected); status_bool(o, "mqtt_connected", snapshot->network.mqtt_connected);
    status_bool(o, "ap_active", snapshot->network.ap_active); status_bool(o, "uart_ready", snapshot->sensor.m1.uart_ready);
    status_bool(o, "sensor_valid", fresh); status_bool(o, "sensor_received", snapshot->sensor.m1.sample.received);
    if (snapshot->sensor.m1.sample.received) json_build_object_add(o, "age_ms",json_object_new_double((uint32_t)(now - snapshot->sensor.m1.sample.updated_ms)));
    else json_object_object_add(o, "age_ms", NULL);
    json_object *co2 = json_object_new_object();
    bool co2_fresh = snapshot->sensor.m1.co2.ready && snapshot->sensor.m1.co2.received &&
        (uint32_t)(now - snapshot->sensor.m1.co2.updated_ms) < 20000;
    status_bool(co2, "ready", snapshot->sensor.m1.co2.ready); status_bool(co2, "valid", co2_fresh);
    status_string(co2, "serial", snapshot->sensor.m1.co2.serial);
    status_int(co2, "errors", snapshot->sensor.m1.co2.errors);
    status_int(co2, "last_error", snapshot->sensor.m1.co2.last_error);
    if (!live) {
        status_int(co2, "samples", full->model.m1.co2_samples);
        status_int(co2, "crc_errors", full->model.m1.co2_crc_errors);
        status_bool(co2, "bus_initialized", full->model.m1.co2_bus_initialized);
        status_int(co2, "last_command", full->model.m1.co2_last_command);
        status_int(co2, "error_phase", full->model.m1.co2_error_phase);
    }
    if (snapshot->sensor.m1.co2.received) status_int(co2, "age_ms", (uint32_t)(now - snapshot->sensor.m1.co2.updated_ms));
    else json_object_object_add(co2, "age_ms", NULL);
    if (co2_fresh) {
        status_int(co2, "co2_ppm", snapshot->sensor.m1.co2.co2_ppm);
    } else {
        json_object_object_add(co2, "co2_ppm", NULL);
    }
    json_build_object_add(o, "scd40", co2);
    status_int(o, "brightness_requested", snapshot->control.m1.brightness_requested);
    if (snapshot->sensor.m1.sample.received) json_build_object_add(o, "temperature",json_object_new_double(snapshot->sensor.m1.sample.temperature_deci_c / 10.0));
    else json_object_object_add(o, "temperature", NULL);
    if (snapshot->sensor.m1.sample.received) json_build_object_add(o, "humidity",json_object_new_double(snapshot->sensor.m1.sample.humidity_deci_pct / 10.0));
    else json_object_object_add(o, "humidity", NULL);
    if (snapshot->sensor.m1.sample.received) json_build_object_add(o, "formaldehyde",json_object_new_double(snapshot->sensor.m1.sample.hcho_ug_m3 / 1000.0));
    else json_object_object_add(o, "formaldehyde", NULL);
    if (snapshot->sensor.m1.sample.received) json_build_object_add(o, "pm25",json_object_new_int(snapshot->sensor.m1.sample.pm25_ug_m3));
    else json_object_object_add(o, "pm25", NULL);
    if (!live) {
        status_int(o, "rx_frames", full->model.m1.rx_frames); status_int(o, "unknown_frames", full->model.m1.unknown_frames);
        status_int(o, "uart_errors", full->model.m1.uart_errors); status_int(o, "discarded_bytes", full->model.m1.discarded_bytes);
        char frame[41];
        for (unsigned i = 0; i < 20; i++) snprintf(frame + i * 2, 3, "%02x", full->model.m1.last_frame[i]);
        status_string(o, "last_frame", frame); status_bool(o,"ota_signature_verified",full->ota.signature_verified);
        status_bool(o,"ota_image_verified",full->ota.image_verified);
        status_int(o,"ota_key_id",full->ota.key_id);status_int(o,"ota_image_length",full->ota.image_length);
        status_string(o,"ota_release_counter",full->ota.release_counter);status_string(o,"ota_version",full->ota.version);
    }
    status_string(o, "ota_state", snapshot->ota.state); status_string(o, "ota_error", snapshot->ota.error);
    status_int(o, "ota_progress", snapshot->ota.progress); status_bool(o, "ota_active", snapshot->ota.active);
    status_bool(o, "scan_busy", snapshot->ota.scan_busy);
    return o;
}

#endif
static __attribute__((noinline)) json_object *encode_tc1(const device_status_t *s,const device_status_full_t *full)
{
    json_object *o=json_object_new_object();if(!o)return NULL;
    if(full){
        status_int(o,"utc_time",full->model.tc1.utc_time);status_bool(o,"time_synced",full->model.tc1.time_synced);
    }
    status_string(o,"wifi_trial_status",s->network.wifi_trial_status);
    status_string(o,"version",s->identity.version);status_string(o,"device_id",s->identity.id);status_string(o,"name",s->identity.name);
    if(full){
        status_string(o,"ssid",full->network.ssid);status_string(o,"ip",full->network.ip);
        status_string(o,"mask",full->model.tc1.mask);status_string(o,"gateway",full->model.tc1.gateway);
        status_string(o,"ap_name",full->network.ap_name);status_string(o,"mqtt_host",full->network.mqtt_host);status_string(o,"mqtt_user",full->network.mqtt_user);
        status_string(o,"mqtt_topic_mode",full->model.tc1.modern?"tmcp":"legacy");
        status_string(o,"mqtt_protocol",full->model.tc1.modern?"tmcp":"legacy");status_int(o,"mqtt_protocol_version",full->model.tc1.modern?2:1);
        json_object *names=json_object_new_array();
        for(unsigned i=0;i<6;i++)json_build_array_add(names,json_object_new_string(full->model.tc1.socket_names[i]));
        json_build_object_add(o,"socket_names",names);
        status_int(o,"mqtt_port",full->network.mqtt_port);
        status_int(o,"report_seconds",full->model.tc1.report_seconds);status_int(o,"powerCoefficientX100",full->model.tc1.power_coefficient);
        if(full->runtime.memory_valid)status_int(o,"free_memory",full->runtime.free_memory);
        status_bool(o,"wifi_password_set",full->network.wifi_password_set);status_bool(o,"mqtt_password_set",full->network.mqtt_password_set);status_bool(o,"web_password_set",full->network.web_password_set);
        phicomm_variant_json(o);
    }
    status_string(o,"model","tc1");status_int(o,"uptime_seconds",s->now_ms/1000);
    json_object *states=json_object_new_array();
    for(unsigned i=0;i<6;i++)json_build_array_add(states,json_object_new_int(s->control.tc1.states[i]));
    json_build_object_add(o,"socket_states",states);
    status_bool(o,"led_enabled",s->control.tc1.led_enabled);status_bool(o,"child_lock",s->control.tc1.child_lock);
    status_string(o,"mqtt_status",device_mqtt_status(s->network.mqtt_status));
    status_bool(o,"wifi_connected",s->network.wifi_connected);status_bool(o,"ap_active",s->network.ap_active);
    status_bool(o,"mqtt_cleanup_warning",s->network.mqtt_cleanup_warning);status_bool(o,"mqtt_connected",s->network.mqtt_connected);
    status_string(o,"ota_state",s->ota.state);status_string(o,"ota_error",s->ota.error);status_int(o,"ota_progress",s->ota.progress);
    return o;
}

json_object *device_status_encode(const device_status_t *s,const device_status_full_t *full)
{
    if(!s || (s->captured&(STATUS_IDENTITY|STATUS_NETWORK|STATUS_OTA))!=(STATUS_IDENTITY|STATUS_NETWORK|STATUS_OTA))return NULL;
    if(full && !(s->captured&STATUS_DIAGNOSTIC))return NULL;
#if defined(PHICOMM_APPLICATION_WATCHDOG)
    if(s->model==DEVICE_A1 && (s->captured&STATUS_CONTROL))return encode_a1(s,full);
    if(s->model==DEVICE_M1 && (s->captured&STATUS_SENSOR))return encode_m1(s,full);
#endif
    if(s->model==DEVICE_TC1 && (s->captured&STATUS_CONTROL))return encode_tc1(s,full);
    return NULL;
}

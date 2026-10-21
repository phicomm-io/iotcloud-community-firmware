/* Frozen status schema oracle: a8ae2a49, prior typed capture. */
static void legacy_str(json_object *o, const char *key, const char *value)
{ json_build_object_add(o, key, json_object_new_string(value)); }
static void legacy_integer(json_object *o, const char *key, int value)
{ json_build_object_add(o, key, json_object_new_int(value)); }
static void legacy_json_bool(json_object *o, const char *key, bool value)
{ json_build_object_add(o, key, json_object_new_boolean(value)); }

static __attribute__((noinline)) json_object *legacy_m1_status_json_view(bool live)
{
    m1_config_t legacy_cfg; m1_state_t s;
    m1_config_get(&legacy_cfg); m1_state_get(&s);
    uint32_t now = mico_rtos_get_time(); bool fresh = m1_sample_fresh(&s.sample, now);
    json_object *o = json_object_new_object();
    if (!o) return NULL;
    legacy_str(o,"model","m1");
    if (!live) {
        phicomm_variant_json(o);
        runtime_metrics_json(o);
        http_diag_t diag = http_diag_snapshot();
        legacy_integer(o, "http_accept_ok", (int)diag.accepted);
        legacy_integer(o, "http_accept_fail", (int)diag.accept_failures);
        legacy_integer(o, "http_accept_errno", diag.accept_errno);
        legacy_integer(o, "http_accept_last_fd", diag.last_fd);
        legacy_integer(o, "http_accept_queue_ok", (int)diag.queued);
        legacy_integer(o, "http_accept_queue_reject", (int)diag.rejected);
        json_object_object_add(o,"http_accept_queue_last_rc",NULL);
        legacy_integer(o, "http_accept_busy_fcntl_fail", (int)diag.nonblock_failures);
        legacy_integer(o, "http_accept_busy_drained", (int)diag.drained);
        legacy_integer(o, "http_accept_busy_send_ok", (int)diag.busy_sent);
        legacy_integer(o, "http_accept_busy_send_fail", (int)diag.busy_failed);
        legacy_integer(o, "http_accept_busy_send_errno", diag.busy_errno);
        legacy_integer(o, "http_accept_busy_send_bytes", (int)diag.busy_bytes);
        legacy_integer(o, "http_accept_overflow_close", (int)diag.rejected);
        legacy_integer(o, "http_accept_queue_capacity", HTTP_PENDING_CLIENTS);
        legacy_integer(o, "http_tcp_limit_requested", HTTP_TCP_CLIENT_LIMIT);
        legacy_integer(o, "http_tcp_limit_set_rc", diag.set_rc);
        legacy_integer(o, "http_tcp_limit_set_errno", diag.set_errno);
        legacy_integer(o, "http_tcp_limit_get_rc", diag.limit_get_rc);
        if (diag.limit_get_rc == 0) json_build_object_add(o, "http_tcp_limit_readback",json_object_new_int(diag.limit));
        else json_object_object_add(o, "http_tcp_limit_readback", NULL);
        legacy_integer(o, "http_tcp_connections_get_rc", diag.connections_get_rc);
        if (diag.connections_get_rc == 0) json_build_object_add(o, "http_tcp_connections",json_object_new_int(diag.connections));
        else json_object_object_add(o, "http_tcp_connections", NULL);
        watchdog_status_json(o);
    } else legacy_integer(o, "uptime_seconds", mico_rtos_get_time()/1000);
    legacy_str(o, "version", M1_VERSION); legacy_str(o, "device_id", m1_id); legacy_str(o, "name", legacy_cfg.name);
    legacy_json_bool(o, "wifi_trial_active", s.wifi_trial_active);
    legacy_str(o, "wifi_trial_status", s.wifi_trial_status);
    if (!live) {
        legacy_str(o, "ssid", legacy_cfg.ssid); legacy_str(o, "ip", s.ip); legacy_str(o, "ap_name", s.ap_name);
        legacy_str(o, "mqtt_host", legacy_cfg.mqtt_host); legacy_integer(o, "mqtt_port", legacy_cfg.mqtt_port); legacy_str(o, "mqtt_user", legacy_cfg.mqtt_user);
        legacy_str(o, "mqtt_topic_mode", "tmcp");
        legacy_str(o, "mqtt_protocol", "tmcp"); legacy_integer(o, "mqtt_protocol_version", 2);
        legacy_integer(o, "mqtt_errors", s.mqtt_errors); legacy_str(o, "mqtt_last_error", s.mqtt_last_error);
        legacy_integer(o, "timezone_minutes", legacy_cfg.timezone_minutes);
        char ntp_host[64]; m1_ntp_host_get(ntp_host);
        legacy_str(o, "ntp_host", ntp_host);
        legacy_str(o, "ntp_status", s.ntp_status[0] ? s.ntp_status : "waiting_network");
        legacy_str(o, "ntp_server", s.ntp_server);
        legacy_json_bool(o, "ntp_synced", s.ntp_synced);
        legacy_json_bool(o, "ntp_screen_queued", s.ntp_screen_queued);
        /* This SDK serializes doubles with %g (six significant digits), which
         * rounds Unix timestamps by minutes. Preserve whole seconds as an legacy_integer. */
        if (s.ntp_synced) json_build_object_add(o, "ntp_last_success",json_object_new_int(s.ntp_last_success));
        else json_object_object_add(o, "ntp_last_success", NULL);
        legacy_integer(o, "ntp_next_seconds", s.wifi_connected && s.ntp_next_at &&
            (int32_t)(s.ntp_next_at - now) > 0 ? (uint32_t)(s.ntp_next_at - now) / 1000 : 0);
        legacy_json_bool(o, "mqtt_password_set", legacy_cfg.mqtt_password[0] != 0);
        legacy_json_bool(o, "web_password_set", legacy_cfg.web_password[0] != 0);
    }
    legacy_str(o, "mqtt_status", mqtt_status_name(s.mqtt_status));
    legacy_json_bool(o, "mqtt_cleanup_warning", s.mqtt_cleanup_warning);
    legacy_json_bool(o, "wifi_connected", s.wifi_connected); legacy_json_bool(o, "mqtt_connected", s.mqtt_connected);
    legacy_json_bool(o, "ap_active", s.ap_active); legacy_json_bool(o, "uart_ready", s.uart_ready);
    legacy_json_bool(o, "sensor_valid", fresh); legacy_json_bool(o, "sensor_received", s.sample.received);
    if (s.sample.received) json_build_object_add(o, "age_ms",json_object_new_double((uint32_t)(now - s.sample.updated_ms)));
    else json_object_object_add(o, "age_ms", NULL);
    json_object *co2 = json_object_new_object();
    bool co2_fresh = s.scd40.ready && s.scd40.received &&
        (uint32_t)(now - s.scd40.updated_ms) < 20000;
    legacy_json_bool(co2, "ready", s.scd40.ready); legacy_json_bool(co2, "valid", co2_fresh);
    legacy_str(co2, "serial", s.scd40.serial);
    legacy_integer(co2, "errors", s.scd40.errors);
    legacy_integer(co2, "last_error", s.scd40.last_error);
    if (!live) {
        legacy_integer(co2, "samples", s.scd40.samples);
        legacy_integer(co2, "crc_errors", s.scd40.crc_errors);
        legacy_json_bool(co2, "bus_initialized", s.scd40.bus_initialized);
        legacy_integer(co2, "last_command", s.scd40.last_command);
        legacy_integer(co2, "error_phase", s.scd40.error_phase);
    }
    if (s.scd40.received) legacy_integer(co2, "age_ms", (uint32_t)(now - s.scd40.updated_ms));
    else json_object_object_add(co2, "age_ms", NULL);
    if (co2_fresh) {
        legacy_integer(co2, "co2_ppm", s.scd40.co2_ppm);
    } else {
        json_object_object_add(co2, "co2_ppm", NULL);
    }
    json_build_object_add(o, "scd40", co2);
    legacy_integer(o, "brightness_requested", s.brightness_requested);
    if (s.sample.received) json_build_object_add(o, "temperature",json_object_new_double(s.sample.temperature_deci_c / 10.0));
    else json_object_object_add(o, "temperature", NULL);
    if (s.sample.received) json_build_object_add(o, "humidity",json_object_new_double(s.sample.humidity_deci_pct / 10.0));
    else json_object_object_add(o, "humidity", NULL);
    if (s.sample.received) json_build_object_add(o, "formaldehyde",json_object_new_double(s.sample.hcho_ug_m3 / 1000.0));
    else json_object_object_add(o, "formaldehyde", NULL);
    if (s.sample.received) json_build_object_add(o, "pm25",json_object_new_int(s.sample.pm25_ug_m3));
    else json_object_object_add(o, "pm25", NULL);
    if (!live) {
        legacy_integer(o, "rx_frames", s.rx_frames); legacy_integer(o, "unknown_frames", s.unknown_frames);
        legacy_integer(o, "uart_errors", s.uart_errors); legacy_integer(o, "discarded_bytes", s.discarded_bytes);
        char frame[41];
        for (unsigned i = 0; i < 20; i++) snprintf(frame + i * 2, 3, "%02x", s.last_frame[i]);
        legacy_str(o, "last_frame", frame); legacy_json_bool(o,"ota_signature_verified",s.ota_signature_verified);
        legacy_json_bool(o,"ota_image_verified",s.ota_image_verified);
        legacy_integer(o,"ota_key_id",s.ota_key_id);legacy_integer(o,"ota_image_length",s.ota_image_length);
        legacy_str(o,"ota_release_counter",s.ota_release_counter);legacy_str(o,"ota_version",s.ota_version);
    }
    legacy_str(o, "ota_state", s.ota_state); legacy_str(o, "ota_error", s.ota_error);
    legacy_integer(o, "ota_progress", s.ota_progress); legacy_json_bool(o, "ota_active", s.ota_active);
    legacy_json_bool(o, "scan_busy", s.scan_busy);
    return o;
}

/* Preserve the legacy full snapshot for existing callers and telemetry. */
json_object *legacy_m1_status_json(void) { return legacy_m1_status_json_view(false); }
json_object *legacy_m1_status_live_json(void) { return legacy_m1_status_json_view(true); }


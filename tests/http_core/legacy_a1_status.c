/* Frozen status schema oracle: a8ae2a49, prior typed capture. */
static void legacy_str(json_object *o, const char *key, const char *value)
{ json_build_object_add(o, key, json_object_new_string(value)); }
static void legacy_integer(json_object *o, const char *key, int value)
{ json_build_object_add(o, key, json_object_new_int(value)); }
static void legacy_json_bool(json_object *o, const char *key, bool value)
{ json_build_object_add(o, key, json_object_new_boolean(value)); }
static __attribute__((noinline)) json_object *legacy_a1_status_json_view(bool live)
{
    a1_config_t cfg; a1_state_t s;
    a1_config_get(&cfg); a1_state_get(&s);
    json_object *o = json_object_new_object();
    if (!o) return NULL;
    legacy_str(o,"model","a1");
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
        json_build_object_add(o, "filter", a1_filter_json());
    } else legacy_integer(o, "uptime_seconds", mico_rtos_get_time()/1000);
    legacy_str(o, "version", A1_VERSION); legacy_str(o, "device_id", a1_id); legacy_str(o, "name", cfg.name);
    legacy_json_bool(o, "wifi_trial_active", s.wifi_trial_active);
    legacy_str(o, "wifi_trial_status", s.wifi_trial_status);
    if (!live) {
        legacy_str(o, "ssid", cfg.ssid); legacy_str(o, "ip", s.ip); legacy_str(o, "ap_name", s.ap_name);
        legacy_str(o, "mqtt_host", cfg.mqtt_host); legacy_integer(o, "mqtt_port", cfg.mqtt_port); legacy_str(o, "mqtt_user", cfg.mqtt_user);
        legacy_str(o, "mqtt_topic_mode", "tmcp");
        legacy_str(o, "mqtt_protocol", "tmcp"); legacy_integer(o, "mqtt_protocol_version", 2);
        legacy_integer(o, "mqtt_errors", s.mqtt_errors); legacy_str(o, "mqtt_last_error", s.mqtt_last_error);
        legacy_json_bool(o, "web_password_set", cfg.web_password[0] != 0);
        legacy_json_bool(o, "wifi_password_set", cfg.wifi_password[0] != 0);
        legacy_json_bool(o, "mqtt_password_set", cfg.mqtt_password[0] != 0);
    }
    legacy_str(o, "mqtt_status", mqtt_status_name(s.mqtt_status));
    legacy_json_bool(o, "mqtt_cleanup_warning", s.mqtt_cleanup_warning);
    legacy_json_bool(o, "wifi_connected", s.wifi_connected); legacy_json_bool(o, "mqtt_connected", s.mqtt_connected);
    bool effective_on; unsigned effective_speed; feature_effective_fan(cfg.fan_on,cfg.speed,&effective_on,&effective_speed);
    legacy_str(o,"fan_mode",feature_fan_auto()?"auto":"manual");
    legacy_str(o,"fan_state",!effective_on?"off":effective_speed?"running":"idle");
    legacy_integer(o,"auto_max_speed",feature_fan_limit());
    legacy_json_bool(o,"requested_fan_on",cfg.fan_on); legacy_integer(o,"requested_speed",cfg.speed);
    legacy_json_bool(o, "ap_active", s.ap_active); legacy_json_bool(o, "fan_on", effective_on);
    legacy_integer(o, "speed", cfg.speed); legacy_integer(o, "percentage", effective_on ? effective_speed : 0);
    legacy_integer(o, "applied_speed", s.applied_speed); legacy_json_bool(o, "fan_ready", s.fan_ready);
    legacy_json_bool(o, "touch_active", s.touch_active);
    legacy_json_bool(o, "led_enabled", cfg.led_enabled); legacy_json_bool(o, "led_visible", s.led_visible);
    legacy_json_bool(o, "led_always_on", cfg.led_always_on); legacy_integer(o, "brightness", cfg.brightness);
    legacy_integer(o, "red", cfg.red); legacy_integer(o, "green", cfg.green); legacy_integer(o, "blue", cfg.blue);
    legacy_json_bool(o, "child_lock", cfg.child_lock); legacy_json_bool(o, "panel_ready", s.panel_ready);
    if (!live) {
        legacy_integer(o, "panel_errors", s.panel_errors); legacy_integer(o, "hardware_errors", s.hardware_errors);
        legacy_integer(o, "panel_last_error", s.panel_last_error);
        char raw[7];
        for (unsigned i = 0; i < 3; i++) snprintf(raw + 2 * i, 3, "%02x", s.panel_raw[i]);
        legacy_str(o, "panel_raw", raw);
    }
    legacy_json_bool(o, "config_dirty", s.config_dirty); legacy_str(o, "save_error", s.save_error);
    if (!live) {
        legacy_json_bool(o,"ota_signature_verified",s.ota_signature_verified);
        legacy_json_bool(o,"ota_image_verified",s.ota_image_verified);
        legacy_integer(o,"ota_key_id",s.ota_key_id);legacy_integer(o,"ota_image_length",s.ota_image_length);
        legacy_str(o,"ota_release_counter",s.ota_release_counter);legacy_str(o,"ota_version",s.ota_version);
    }
    legacy_str(o, "ota_state", s.ota_state); legacy_str(o, "ota_error", s.ota_error);
    legacy_integer(o, "ota_progress", s.ota_progress); legacy_json_bool(o, "ota_active", s.ota_active);
    legacy_json_bool(o, "reboot_pending", s.reboot_pending); legacy_json_bool(o, "scan_busy", s.scan_busy);
    return o;
}

/* Preserve the legacy full snapshot for existing callers and telemetry. */
json_object *legacy_a1_status_json(void) { return legacy_a1_status_json_view(false); }
json_object *legacy_a1_status_live_json(void) { return legacy_a1_status_json_view(true); }


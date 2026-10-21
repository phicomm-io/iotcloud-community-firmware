/* Frozen pre-snapshot production schema oracle (a77026de). */
json_object *legacy_tc1_status_json(void) {
    unsigned long free_memory = (unsigned long)MicoGetMemoryInfo()->free_memory;
    /* Keep the v1 command status free of obsolete duplicate fields. */
    size_t capacity = 2048;
    capacity += (jb_string_size(sys_config->micoSystemConfig.name) +
                     jb_string_size(sys_config->micoSystemConfig.ssid) +
                     jb_string_size(user_config->ap_name) + jb_string_size(MQTT_SERVER) +
                     jb_string_size(MQTT_SERVER_USR));
    for (int i = 0; i < SOCKET_NUM; ++i)
        capacity += jb_string_size(user_config->socket_names[i]);
    char *data = malloc(capacity);
    if (!data)
        return NULL;
    JsonBuffer b = {data, 0, capacity, true};
    mico_time_t now;
    mico_time_get_time(&now);
    jb_printf(&b, "{");
    jb_printf(&b, "\"utc_time\":%ld,\"time_synced\":%s,", (long)time(NULL),
              rtc_init == 1 ? "true" : "false");
    jb_field(&b, "wifi_trial_status", WifiTrialStatus());
    jb_field(&b, "version", VERSION);
    jb_field(&b, "device_id", str_mac);
    jb_field(&b, "name", sys_config->micoSystemConfig.name);
    jb_field(&b, "ssid", sys_config->micoSystemConfig.ssid);
    jb_field(&b, "ip", ip_status.ip);
    jb_field(&b, "mask", ip_status.mask);
    jb_field(&b, "gateway", ip_status.gateway);
    jb_field(&b, "ap_name", user_config->ap_name);
    jb_field(&b, "mqtt_host", MQTT_SERVER);
    jb_field(&b, "mqtt_user", MQTT_SERVER_USR);
    jb_field(&b, "mqtt_topic_mode", user_config->mqtt_topic_mode ? "tmcp" : "legacy");
    jb_field(&b, "mqtt_protocol", user_config->mqtt_topic_mode ? "tmcp" : "legacy");
    jb_printf(&b, "\"mqtt_protocol_version\":%d,", user_config->mqtt_topic_mode ? 2 : 1);
    jb_field(&b, "mqtt_status", mqtt_status_name(tc1_mqtt_status));
    jb_printf(&b, "\"socket_names\":[");
    for (int i = 0; i < SOCKET_NUM; ++i) {
        if (i)
            jb_printf(&b, ",");
        jb_string(&b, user_config->socket_names[i]);
    }
    jb_printf(&b, "],");
    jb_printf(&b, "\"socket_states\":[%s],", GetSocketStatus());
    jb_printf(&b, "\"mqtt_port\":%d,\"led_enabled\":%s,\"child_lock\":%s,",
              MQTT_SERVER_PORT, user_config->power_led_enabled ? "true" : "false",
              childLockEnabled ? "true" : "false");
    jb_printf(&b, "\"report_seconds\":%d,\"powerCoefficientX100\":%u,\"uptime_seconds\":%lu,\"free_memory\":%lu,",
              MQTT_REPORT_FREQ, user_config->power_coefficient_x100,
              (unsigned long)now / 1000, free_memory);
    jb_printf(&b,
              "\"wifi_connected\":%s,\"ap_active\":%s,\"wifi_password_set\":%s,\"mqtt_password_"
              "set\":%s,\"web_password_set\":%s,\"mqtt_cleanup_warning\":%s,\"mqtt_connected\":%s,",
              wifi_status == WIFI_STATE_CONNECTED ? "true" : "false",
              ip_status.mode == 0 ? "true" : "false",
              sys_config->micoSystemConfig.user_key[0] ? "true" : "false",
              user_config->mqtt_password[0] ? "true" : "false",
              user_config->web_password[0] ? "true" : "false",
              tc1_mqtt_cleanup_warning ? "true" : "false",
              tc1_mqtt_status == MQTT_READY ? "true" : "false");
    char state[32], error[128];
    unsigned progress;
    tc1_ota_snapshot(state, error, &progress);
    jb_field(&b, "ota_state", state);
    jb_field(&b, "ota_error", error);
    jb_printf(&b, "\"ota_progress\":%u}", progress);
    json_object *out = b.valid ? service_parse(b.data,b.used) : NULL;
    free(data);
    if(out){phicomm_variant_json(out);json_build_object_add(out,"model",json_object_new_string("tc1"));}
    return out;
}

/* Build only fields needed by high-frequency UI reads: no full buffer/parse. */
json_object *legacy_tc1_status_live_json(void) {
    json_object *o=json_object_new_object(),*states=json_object_new_array();
    if(!o){if(states)json_object_put(states);return NULL;}
    json_build_object_add(o,"model",json_object_new_string("tc1"));
    json_build_object_add(o,"version",json_object_new_string(VERSION));
    json_build_object_add(o,"device_id",json_object_new_string(str_mac));
    json_build_object_add(o,"name",json_object_new_string(sys_config->micoSystemConfig.name));
    mico_time_t now;mico_time_get_time(&now);
    json_build_object_add(o,"uptime_seconds",json_object_new_int((unsigned long)now/1000));
    for(unsigned i=0;i<SOCKET_NUM;i++)json_build_array_add(states,json_object_new_int(user_config->socket_status[i]));
    json_build_object_add(o,"socket_states",states);
    json_build_object_add(o,"child_lock",json_object_new_boolean(childLockEnabled));
    json_build_object_add(o,"led_enabled",json_object_new_boolean(user_config->power_led_enabled));
    json_build_object_add(o,"wifi_connected",json_object_new_boolean(wifi_status==WIFI_STATE_CONNECTED));
    json_build_object_add(o,"mqtt_connected",json_object_new_boolean(tc1_mqtt_status==MQTT_READY));
    json_build_object_add(o,"ap_active",json_object_new_boolean(ip_status.mode==0));
    json_build_object_add(o,"mqtt_cleanup_warning",json_object_new_boolean(tc1_mqtt_cleanup_warning));
    json_build_object_add(o,"mqtt_status",json_object_new_string(mqtt_status_name(tc1_mqtt_status)));
    json_build_object_add(o,"wifi_trial_status",json_object_new_string(WifiTrialStatus()));
    char state[32],error[128];unsigned progress;tc1_ota_snapshot(state,error,&progress);
    json_build_object_add(o,"ota_state",json_object_new_string(state));
    json_build_object_add(o,"ota_error",json_object_new_string(error));
    json_build_object_add(o,"ota_progress",json_object_new_int(progress));
    return o;
}


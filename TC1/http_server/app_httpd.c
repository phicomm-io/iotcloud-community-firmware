#include "../../common/ota/signed_ota.h"
#include "ota_server/ota_server.h"
#include "ota_server/signed_adapter.h"
/**
 ******************************************************************************
 * @file    app_https.c
 * @author  QQ DING
 * @version V1.0.0
 * @date    1-September-2015
 * @brief   The main HTTPD server initialization and wsgi handle.
 ******************************************************************************
 *
 *  The MIT License
 *  Copyright (c) 2016 MXCHIP Inc.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is furnished
 *  to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 *  WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
 *  IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 ******************************************************************************
 */

#include <time.h>
#include <httpd.h>
#include <http_parse.h>
#include <http-strings.h>
#include "stdlib.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mico.h"
#include "httpd_priv.h"
#include "app_httpd.h"
#include "user_gpio.h"
#include "user_wifi.h"
#include "user_power.h"
#include "main.h"
#include "web_data.c"
#include "web_log.h"
#include "timed_task/timed_task.h"
#include "ota_server/user_ota.h"
#include "mqtt_server/user_mqtt_client.h"
#include "json.h"

static bool is_http_init;
static bool is_handlers_registered;
const struct httpd_wsgi_call g_app_handlers[];
char power_info_json[2560] = {0};
char up_time[16] = "00:00:00";
#define CHUNK_SIZE 512  // 姣忔鍙戦€?512 瀛楄妭锛岄伩鍏?buffer 澶ぇ
#define OTA_BUFFER_SIZE 512
#define MAX_OTA_SIZE 1024*1024

/*
void GetPraFromUrl(char* url, char* pra, char* val)
{
    char* sub = strstr(url, pra);
    if (sub == NULL)
    {
        val[0] = 0;
        return;
    }
    sub = strstr(sub, "=");
    if (sub == NULL)
    {
        val[0] = 0;
        return;
    }
    int len = strlen(sub);
    int n = 0;
    for (int i = 0; i < len; i++)
    {
        if (sub[i] == '&' || i == len - 1)
        {
            n = len;
            break;
        }
    }
    if (n > 0)
    {
        strncpy(val, sub + 1, n - 1);
        val[n - 1] = 0;
        return;
    }
    val[0] = 0;
}
*/


static OSStatus send_in_chunks(int sock, const uint8_t *data, int total_len) {
    OSStatus err = kNoErr;
    for (int offset = 0; offset < total_len; offset += CHUNK_SIZE) {
        int chunk_len = (total_len - offset > CHUNK_SIZE) ? CHUNK_SIZE : (total_len - offset);
        err = httpd_send_body(sock, data + offset, chunk_len);
        require_noerr_action(err, exit, http_log("ERROR: Send chunk failed at offset %d", offset));
    }
exit:
    return err;
}

static int HttpGetIndexPage(httpd_request_t *req) {
    OSStatus err = kNoErr;
    int total_sz = sizeof(web_index_html);


    err = httpd_send_all_header(req, HTTP_RES_200, total_sz, HTTP_CONTENT_HTML_ZIP);
    require_noerr_action(err, exit, http_log("ERROR: Unable to send index headers."));

    err = send_in_chunks(req->sock, web_index_html, total_sz);
    require_noerr_action(err, exit, http_log("ERROR: Unable to send index body."));

exit:
    return err;
}

static int HttpGetAssets(httpd_request_t *req) {
    OSStatus err = kNoErr;

    char *file_name = strstr(req->filename, "/assets/");
    if (!file_name) {
        http_log("HttpGetAssets url[%s] err", req->filename);
        return err;
    }

    int total_sz = 0;
    const unsigned char *file_data = NULL;
    const char *content_type = HTTP_CONTENT_JS_ZIP;

    if (strcmp(file_name + 8, "js_pack.js") == 0) {
        total_sz = sizeof(js_pack);
        file_data = js_pack;
    } else if (strcmp(file_name + 8, "css_pack.css") == 0) {
        total_sz = sizeof(css_pack);
        file_data = css_pack;
        content_type = HTTP_CONTENT_CSS_ZIP;
    } else if (strcmp(file_name + 8, "index.html") == 0) {
        total_sz = sizeof(web_index_html);
        file_data = web_index_html;
        content_type = HTTP_CONTENT_HTML_ZIP;
    }

    if (total_sz == 0 || file_data == NULL) {
        http_log("File not found: %s", req->filename);
        return err;
    }


    err = httpd_send_all_header(req, HTTP_RES_200, total_sz, content_type);
    require_noerr_action(err, exit, http_log("ERROR: Unable to send asset headers."));

    err = send_in_chunks(req->sock, file_data, total_sz);
    require_noerr_action(err, exit, http_log("ERROR: Unable to send asset body."));

exit:
    return err;
}

static int HttpGetTc1Status(httpd_request_t *req) {
    json_object *status = json_object_new_object();
    if (!status) return kGeneralErr;
    char socket_names[512], number[16];
    snprintf(socket_names, sizeof(socket_names), "%s,%s,%s,%s,%s,%s",
             user_config->socket_names[0], user_config->socket_names[1],
             user_config->socket_names[2], user_config->socket_names[3],
             user_config->socket_names[4], user_config->socket_names[5]);
#define STATUS_STRING(key, value) json_object_object_add(status, key, json_object_new_string(value))
#define STATUS_INT(key, value) json_object_object_add(status, key, json_object_new_int(value))
    STATUS_STRING("sockets", GetSocketStatus());
    STATUS_INT("mode", ip_status.mode);
    STATUS_STRING("station_ssid", sys_config->micoSystemConfig.ssid);
    STATUS_STRING("station_pwd", sys_config->micoSystemConfig.user_key);
    STATUS_STRING("ap_ssid", user_config->ap_name);
    STATUS_STRING("ap_pwd", user_config->ap_key);
    STATUS_STRING("mqtt_addr", MQTT_SERVER);
    STATUS_INT("mqtt_port", MQTT_SERVER_PORT);
    STATUS_STRING("mqtt_usr", MQTT_SERVER_USR);
    STATUS_STRING("mqtt_pwd", "");
    STATUS_STRING("version", VERSION);
    STATUS_STRING("ip", ip_status.ip);
    STATUS_STRING("mask", ip_status.mask);
    STATUS_STRING("gateway", ip_status.gateway);
    snprintf(number, sizeof(number), "%u", user_config->mqtt_report_freq);
    STATUS_STRING("reportFreq", number);
    STATUS_INT("ledEnabled", user_config->power_led_enabled);
    STATUS_INT("up_time", 0);
    STATUS_STRING("socketNames", socket_names);
    STATUS_INT("child_lock_enabled", childLockEnabled);
    STATUS_STRING("deviceName", sys_config->micoSystemConfig.name);
    STATUS_INT("powerCoefficientX100", user_config->power_coefficient_x100);
    char clicks[512];
    snprintf(clicks, sizeof(clicks), "%s", GetButtonClickConfig());
    for (char *c = clicks; *c; ++c) if (*c == 39) *c = 34;
    json_object_object_add(status, "btnClicks", json_tokener_parse(clicks));
    STATUS_STRING("mqtt_topic_mode", user_config->mqtt_topic_mode ? "account" : "legacy");
    STATUS_STRING("mqtt_status", mqtt_status_name(tc1_mqtt_status));
    json_object_object_add(status, "mqtt_password_set", json_object_new_boolean(user_config->mqtt_password[0] != 0));
    json_object_object_add(status, "mqtt_cleanup_warning", json_object_new_boolean(tc1_mqtt_cleanup_warning));
#undef STATUS_STRING
#undef STATUS_INT
    const char *body = json_object_to_json_string(status);
    OSStatus err = kNoErr;
    send_http(body, strlen(body), exit, &err);
exit:
    json_object_put(status);
    return err;
}

static int HttpSetSocketStatus(httpd_request_t *req) {
    OSStatus err = kNoErr;

    int buf_size = 512;
    char *buf = malloc(buf_size);

    err = httpd_get_data(req, buf, buf_size);
    require_noerr(err, exit);

    SetSocketStatus(buf);

    send_http("OK", 2, exit, &err);

    exit:
    if (buf) free(buf);
    return err;
}

static int HttpSetSocketName(httpd_request_t *req) {
    OSStatus err = kNoErr;

    int buf_size = 70;
    char *buf = malloc(buf_size);

    err = httpd_get_data(req, buf, buf_size);
    require_noerr(err, exit);
    int index;
    char name[64];
    sscanf(buf, "%d %s", &index, name);
    strcpy(user_config->socket_names[index], name);
    mico_system_context_update(sys_config);
    registerMqttEvents();
    send_http("OK", 2, exit, &err);

    exit:
    if (buf) free(buf);
    return err;
}

static int HttpSetButtonEvent(httpd_request_t *req) {
    OSStatus err = kNoErr;

    int buf_size = 10;
    char *buf = malloc(buf_size);

    err = httpd_get_data(req, buf, buf_size);
    require_noerr(err, exit);
    int index;
    int func;
    int longPress;
    sscanf(buf, "%d %d %d", &index, &func, &longPress);
    if (longPress == 1) {
        set_key_map(user_config->user,index, get_short_func(user_config->user[index]), func == -1 ? NO_FUNCTION : func);
    } else {
        set_key_map(user_config->user,index, func == -1 ? NO_FUNCTION : func, get_long_func(user_config->user[index]));
    }
    key_log("WARNGIN:set KEY func %d %d %d", index,get_short_func(user_config->user[index]),get_long_func(user_config->user[index]));
    mico_system_context_update(sys_config);

    send_http("OK", 2, exit, &err);

    exit:
    if (buf) free(buf);
    return err;
}

#define OTA_BUF_SIZE 5120

static int HttpUploadOTA(httpd_request_t *req, bool rescue)
{
    OSStatus err=kGeneralErr; char buffer[512];
    if (req->chunked || req->remaining_bytes<=0 || !tc1_ota_reserve()) return kParamErr;
    if (!tc1_ota_begin(req->remaining_bytes,rescue)) {tc1_ota_abort();return kParamErr;}
    while (req->remaining_bytes>0) {
        int n=httpd_get_data2(req,buffer,sizeof(buffer));
        if (n<=0 || !tc1_ota_write((uint8_t *)buffer,n)) { tc1_ota_abort(); return kConnectionErr; }
    }
    if (!tc1_ota_finish()) {tc1_ota_abort();return kGeneralErr;}
    send_http("OTA verified; rebooting",22,exit,&err);
    mico_system_power_perform(mico_system_context_get(),eState_Software_Reset);
exit:
    return err;
}
static int HttpSetOTAFile(httpd_request_t *req) { return HttpUploadOTA(req,false); }
static int HttpSetOTAUnsigned(httpd_request_t *req) { return HttpUploadOTA(req,true); }

static int HttpSetDeviceName(httpd_request_t *req) {
    OSStatus err = kNoErr;

    int buf_size = 70;
    char *buf = malloc(buf_size);

    err = httpd_get_data(req, buf, buf_size);
    require_noerr(err, exit);
    char name[64];
    sscanf(buf, "%s", name);
    strcpy(sys_config->micoSystemConfig.name, name);
    mico_system_context_update(sys_config);
    registerMqttEvents();
    send_http("OK", 2, exit, &err);

    exit:
    if (buf) free(buf);
    return err;
}

static int HttpSetChildLock(httpd_request_t *req) {
    OSStatus err = kNoErr;

    int buf_size = 32;
    char *buf = malloc(buf_size);

    err = httpd_get_data(req, buf, buf_size);
    require_noerr(err, exit);
    int enableLock;
    sscanf(buf, "%d", &enableLock);
    user_config->user[0] = enableLock;
    childLockEnabled = enableLock;
    mico_system_context_update(sys_config);
    UserMqttSendChildLockState();
    send_http("OK", 2, exit, &err);

    exit:
    if (buf) free(buf);
    return err;
}

static int HttpGetPowerInfo(httpd_request_t *req) {
    OSStatus err = kNoErr;
    char buf[16];
    err = httpd_get_data(req, buf, 16);
    require_noerr(err, exit);

    int idx = 0;
    sscanf(buf, "%d", &idx);

    //璁＄畻绯荤粺杩愯鏃堕棿
    mico_time_t past_ms = 0;
    mico_time_get_time(&past_ms);
    int past = past_ms / 1000;
    int d = past / 3600 / 24;
    int h = past / 3600 % 24;
    int m = past / 60 % 60;
    int s = past % 60;
    sprintf(up_time, "%d - %02d:%02d:%02d", d, h, m, s);

    char *powers = GetPowerRecord(idx);
    char *sockets = GetSocketStatus();
    char *short_click_config = GetButtonClickConfig();
    char *socket_names = malloc(512);
    sprintf(socket_names, "%s,%s,%s,%s,%s,%s",
            user_config->socket_names[0],
            user_config->socket_names[1],
            user_config->socket_names[2],
            user_config->socket_names[3],
            user_config->socket_names[4],
            user_config->socket_names[5]);
    sprintf(power_info_json, POWER_INFO_JSON, sockets, power_record.idx, PW_NUM, p_count, powers,
            up_time, user_config->power_led_enabled, RelayOut() ? 1 : 0, socket_names,
            user_config->p_count_1_day_ago, user_config->p_count_2_days_ago, childLockEnabled,
            sys_config->micoSystemConfig.name,
            (unsigned int) user_config->power_coefficient_x100, short_click_config);
    send_http(power_info_json, strlen(power_info_json), exit, &err);
    if (socket_names) free(socket_names);
    exit:
    return err;
}

static int HttpGetWifiConfig(httpd_request_t *req) {
    OSStatus err = kNoErr;
    char *status = "test";
    send_http(status, strlen(status), exit, &err);
    exit:
    return err;
}


// 鍗曚釜鍗佸叚杩涘埗瀛楃杞暟瀛楋紙瀹夊叏锛?
static int hex_char_to_int(char c) {
    if ('0' <= c && c <= '9') return c - '0';
    if ('a' <= c && c <= 'f') return c - 'a' + 10;
    if ('A' <= c && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 鍋ュ．鐗?URL 瑙ｇ爜鍑芥暟
void url_decode(const char *src, char *dest, size_t max_len) {
    size_t i = 0;
    while (*src && i < max_len - 1) {
        if (*src == '%') {
            if (isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
                int high = hex_char_to_int(src[1]);
                int low = hex_char_to_int(src[2]);
                if (high >= 0 && low >= 0) {
                    dest[i++] = (char)((high << 4) | low);
                    src += 3;
                    continue;
                }
            }
            // 闈炴硶缂栫爜锛岃烦杩?%
            src++;
        } else if (*src == '+') {
            dest[i++] = ' ';
            src++;
        } else {
            dest[i++] = *src++;
        }
    }
    dest[i] = '\0';
}

static int HttpSetWifiConfig(httpd_request_t *req) {
    OSStatus err = kNoErr;

  char *buf = malloc(256);
  char *ssid_enc = malloc(128);
  char *key_enc = malloc(128);
  char *wifi_ssid = malloc(128);
  char *wifi_key = malloc(128);
  int mode = -1;
    require_action(buf && ssid_enc && key_enc && wifi_ssid && wifi_key, exit, err = kNoMemoryErr);



    err = httpd_get_data(req, buf, 256);
    require_noerr(err, exit);
  // 鍋囪 httpd_get_data(req, buf, 256);
//  tc1_log("wifi config %s",buf);
  if (sscanf(buf, "%d %127s %127s", &mode, ssid_enc, key_enc) != 3) {
      err = kParamErr;
      goto exit;
  }
//  tc1_log("wifi config %s %s",ssid_enc,key_enc);
  url_decode(ssid_enc, wifi_ssid,128);
  url_decode(key_enc, wifi_key,128);
//  tc1_log("wifi config decode %s %s",wifi_ssid,wifi_key);
    if (mode == 1) {
        WifiConnect(wifi_ssid, wifi_key);
    } else {
        ApConfig(wifi_ssid, wifi_key);
    }

    send_http("OK", 2, exit, &err);

    exit:
    if (buf) free(buf);
    if (ssid_enc) free(ssid_enc);
    if (key_enc) free(key_enc);
    if (wifi_ssid) free(wifi_ssid);
    if (wifi_key) free(wifi_key);
    return err;
}

static int HttpGetWifiScan(httpd_request_t *req) {
    OSStatus err = kNoErr;
    if (scaned) {
        scaned = false;
        send_http(wifi_ret, strlen(wifi_ret), exit, &err);
        free(wifi_ret);
        wifi_ret = NULL;
    } else {
        send_http("NO", 2, exit, &err);
    }

    exit:
    return err;
}

static int HttpSetWifiScan(httpd_request_t *req) {
    micoWlanStartScanAdv();
    OSStatus err = kNoErr;
    send_http("OK", 2, exit, &err);
    exit:
    return err;
}

static int HttpSetRebootSystem(httpd_request_t *req) {
    OSStatus err = kNoErr;
    char buf[10] = {0};
    httpd_get_data(req, buf, sizeof(buf));

    send_http("OK", 2, exit, &err);

    if (strstr(buf, "soft") != NULL) {
        UserSoftReboot();
    } else {
        MicoSystemReboot();
    }

    exit:
    return err;
}

static bool mqtt_json_string(json_object *o, const char *key, char *out, size_t capacity, bool keep_blank) {
    json_object *v=json_object_object_get(o,key);
    if(!v) return true;
    if(!json_object_is_type(v,json_type_string)) return false;
    const char *text=json_object_get_string(v);
    if(strlen(text)!=(size_t)json_object_get_string_len(v) || strlen(text)>=capacity || strpbrk(text,"\r\n")) return false;
    if(*text || !keep_blank) strcpy(out,text);
    return true;
}
static int HttpSetMqttConfig(httpd_request_t *req) {
    OSStatus err=kNoErr;
    char buf[2048]={0}, host[32], user[64], password[257];
    strcpy(host,user_config->mqtt_ip); strcpy(user,user_config->mqtt_user); strcpy(password,user_config->mqtt_password);
    int port=user_config->mqtt_port, mode=user_config->mqtt_topic_mode;
    bool valid=false; json_object *o=NULL;
    err=httpd_get_data(req,buf,sizeof(buf)-1); require_noerr(err,exit);
    if(req->body_nbytes>=sizeof(buf) || memchr(buf,0,req->body_nbytes)) goto invalid;
    if(buf[0]=='{') {
        o=json_tokener_parse(buf);
        if(!o || !json_object_is_type(o,json_type_object)) goto invalid;
        valid=mqtt_json_string(o,"host",host,sizeof(host),false) && mqtt_json_string(o,"user",user,sizeof(user),false) && mqtt_json_string(o,"password",password,sizeof(password),true);
        json_object *v=json_object_object_get(o,"port");
        if(v) { if(!json_object_is_type(v,json_type_int)) valid=false; else port=json_object_get_int(v); }
        v=json_object_object_get(o,"topic_mode");
        if(v) {
            if(!json_object_is_type(v,json_type_string) || json_object_get_string_len(v)!=(int)strlen(json_object_get_string(v)) || (strcmp(json_object_get_string(v),"legacy") && strcmp(json_object_get_string(v),"account"))) valid=false;
            else mode=!strcmp(json_object_get_string(v),"account");
        }
        v=json_object_object_get(o,"clear_password");
        if(v && json_object_is_type(v,json_type_boolean) && json_object_get_boolean(v)) password[0]=0;
    } else {
        /* Compatibility for old offline pages: bounded tokens; reject excess. */
        char extra;
        valid=sscanf(buf,"%31s %d %63s %256s %c",host,&port,user,password,&extra)==4;
    }
    if(!valid || port<1 || port>65535 || (mode && !*user)) goto invalid;
    mico_rtos_suspend_all_thread();
    strcpy(user_config->mqtt_ip,host); strcpy(user_config->mqtt_user,user); strcpy(user_config->mqtt_password,password);
    user_config->mqtt_port=port; user_config->mqtt_topic_mode=mode;
    mico_rtos_resume_all_thread();
    err=mico_system_context_update(sys_config); require_noerr(err,exit);
    err=UserMqttInit(); require_noerr(err,exit);
    send_http("OK",2,exit,&err); goto exit;
invalid:
    send_http("INVALID_MQTT_CONFIG",19,exit,&err);
exit:
    if(o) json_object_put(o);
    return err;
}

static int HttpSetMqttReportFreq(httpd_request_t *req) {
    OSStatus err = kNoErr;

    int buf_size = 97;
    char *buf = malloc(buf_size);

    err = httpd_get_data(req, buf, buf_size);
    require_noerr(err, exit);

    sscanf(buf, "%d", &MQTT_REPORT_FREQ);
    mico_system_context_update(sys_config);

    send_http("OK", 2, exit, &err);

    exit:
    if (buf) free(buf);
    return err;
}

static int HttpGetMqttReportFreq(httpd_request_t *req) {
    OSStatus err = kNoErr;
    int buf_size = 97;
    char *freq = malloc(buf_size);
    sprintf(freq, "%d", MQTT_REPORT_FREQ);

    send_http(freq, strlen(freq), exit, &err);

    exit:
    if (freq) free(freq);
    return err;
}

static int HttpGetLog(httpd_request_t *req) {
    OSStatus err = kNoErr;
    char *logs = GetLogRecord();
    send_http(logs, strlen(logs), exit, &err);

    exit:
    return err;
}

static int HttpGetTasks(httpd_request_t *req) {
    OSStatus err = kNoErr;
    char *tasks_str = GetTaskStr();
    send_http(tasks_str, strlen(tasks_str), exit, &err);

    exit:
    if (tasks_str) free(tasks_str);
    return err;
}

static int HttpGetButtonEvents(httpd_request_t *req) {
    OSStatus err = kNoErr;
    char *clicks = GetButtonClickConfig();
    send_http(clicks, strlen(clicks), exit, &err);

    exit:
    return err;
}

static int HttpAddTask(httpd_request_t *req) {
    OSStatus err = kNoErr;

    //1577369623 4 0
    char buf[20] = {0};
    err = httpd_get_data(req, buf, 20);
    require_noerr(err, exit);

    pTimedTask task = NewTask();
    if (task == NULL) { http_log("NewTask() error, max task num = %d!", MAX_TASK_NUM);
        char *mess = "NO SPACE";
        send_http(mess, strlen(mess), exit, &err);
        return err;
    }
    int re = sscanf(buf, "%ld %d %d %d", &task->prs_time, &task->operation, &task->on,
                    &task->weekday);http_log("AddTask buf[%s] re[%d] (%ld %d %d %d)",
                                             buf, re, task->prs_time, task->operation, task->on,
                                             task->weekday);
    if (task->prs_time < 1577428136 || task->prs_time > 9577428136
        || task->operation < 0 || task->operation > 11) { http_log("AddTask Error!");
        re = 0;
    }

    char *mess = (re == 4 && AddTask(task)) ? "OK" : "NO";

    send_http(mess, strlen(mess), exit, &err);

    exit:
    return err;
}

static int HttpDelTask(httpd_request_t *req) {
    OSStatus err = kNoErr;

    char *time_str = strstr(req->filename, "/task/");
    if (!time_str) { http_log("HttpDelTask url[%s] err", req->filename);
        return err;
    }http_log("HttpDelTask url[%s] time_str[%s][%s]", req->filename, time_str, time_str + 6);

    int time1;
    sscanf(time_str + 6, "%d", &time1);

    char *mess = DelTask(time1) ? "OK" : "NO";

    send_http(mess, strlen(mess), exit, &err);
    exit:
    return err;
}

static int LedStatus(httpd_request_t *req) {
    OSStatus err = kNoErr;
    int buf_size = 97;
    char *led = malloc(buf_size);
    sprintf(led, "%d", MQTT_LED_ENABLED);

    send_http(led, strlen(led), exit, &err);

    exit:
    if (led) free(led);
    return err;
}

static int LedSetEnabled(httpd_request_t *req) {
    OSStatus err = kNoErr;

    int buf_size = 97;
    char *buf = malloc(buf_size);

    err = httpd_get_data(req, buf, buf_size);
    require_noerr(err, exit);

    sscanf(buf, "%d", &MQTT_LED_ENABLED);
    if (RelayOut() && MQTT_LED_ENABLED) {
        UserLedSet(1);
    } else {
        UserLedSet(0);
    }
    UserMqttSendLedState();
    mico_system_context_update(sys_config);

    send_http("OK", 2, exit, &err);

    exit:
    if (buf) free(buf);
    return err;
}

static int TotalSocketSetEnabled(httpd_request_t *req) {
    OSStatus err = kNoErr;

    int buf_size = 97;
    int on;
    char *buf = malloc(buf_size);

    err = httpd_get_data(req, buf, buf_size);
    require_noerr(err, exit);

    sscanf(buf, "%d", &on);
    UserRelaySetAll(on);
    int i = 0;
    for (; i < SOCKET_NUM; i++) {
        UserMqttSendSocketState(i);
    }
    UserMqttSendTotalSocketState();
    send_http("OK", 2, exit, &err);

    exit:
    if (buf) free(buf);
    return err;
}

static int Otastatus(httpd_request_t *req) {
    OSStatus err = kNoErr;
    char buf[16] = {0};
    sprintf(buf, "%.2f", ota_progress);
    send_http(buf, strlen(buf), exit, &err);
    exit:
    return err;
}

static int OtaDetailedStatus(httpd_request_t *req) {
    OSStatus err=kNoMemoryErr;char state[32],error[128],counter[21];unsigned progress;
    tc1_ota_metadata_t meta;tc1_ota_metadata(&meta);tc1_ota_snapshot(state,error,&progress);
    json_object *o=json_object_new_object();if(!o)return err;
    json_object_object_add(o,"state",json_object_new_string(state));
    json_object_object_add(o,"error",json_object_new_string(error));
    json_object_object_add(o,"progress",json_object_new_int(progress));
    json_object_object_add(o,"signature_verified",json_object_new_boolean(meta.signature_verified));
    json_object_object_add(o,"image_verified",json_object_new_boolean(meta.image_verified));
    json_object_object_add(o,"key_id",json_object_new_int(meta.key_id));
    json_object_object_add(o,"image_length",json_object_new_int(meta.image_length));
    json_object_object_add(o,"version",json_object_new_string(meta.version));
    ota_counter_decimal(meta.release_counter,counter);
    json_object_object_add(o,"release_counter",json_object_new_string(counter));
    const char *text=json_object_to_json_string(o);
    send_http(text,strlen(text),exit,&err);
exit:json_object_put(o);return err;
}
static int OtaStart(httpd_request_t *req) {
    OSStatus err=kNoErr;char buf[500]={0};
    if(req->chunked || req->remaining_bytes<=0 || req->remaining_bytes>=sizeof(buf))return kParamErr;
    err=httpd_get_data(req,buf,req->remaining_bytes);
    require_noerr(err,exit);
    err=ota_server_start(buf,NULL,NULL);require_noerr(err,exit);
    send_http("OK",2,exit,&err);
exit:
    return err;
}

static int HttpSetPowerCoefficient(httpd_request_t *req) {
    OSStatus err = kNoErr;
    char buf[16] = {0};
    char extra;
    int coefficient_x100;

    err = httpd_get_data(req, buf, sizeof(buf) - 1);
    require_noerr(err, exit);

    if (sscanf(buf, "%d %c", &coefficient_x100, &extra) != 1 ||
        coefficient_x100 < POWER_COEFFICIENT_MIN_X100 ||
        coefficient_x100 > POWER_COEFFICIENT_MAX_X100) {
        send_http("INVALID_COEFFICIENT", 19, exit, &err);
        goto exit;
    }

    user_config->power_coefficient_x100 = (uint16_t) coefficient_x100;
    err = mico_system_context_update(sys_config);
    require_noerr(err, exit);
    send_http("OK", 2, exit, &err);

exit:
    return err;
}

static int HttpSetWebPassword(httpd_request_t *req) {
    OSStatus err = kNoErr;
    char password[WEB_PASSWORD_LENGTH_MAX + 2] = {0};

    if (!req->hdr_parsed) {
        char *header_buf = malloc(HTTPD_MAX_MESSAGE);
        require_action(header_buf, exit, err = kNoMemoryErr);
        err = httpd_parse_hdr_tags(req, req->sock, header_buf, HTTPD_MAX_MESSAGE);
        free(header_buf);
        require_noerr(err, exit);
        req->hdr_parsed = 1;
    }

    if (req->body_nbytes > WEB_PASSWORD_LENGTH_MAX) {
        send_http("PASSWORD_TOO_LONG", 17, exit, &err);
        goto exit;
    }
    if (req->body_nbytes > 0) {
        err = httpd_get_data(req, password, sizeof(password) - 1);
        require_noerr(err, exit);
    }

    size_t length = strlen(password);
    if (length != 0 && length < 8) {
        send_http("PASSWORD_TOO_SHORT", 18, exit, &err);
        goto exit;
    }
    for (size_t i = 0; i < length; i++) {
        if ((unsigned char) password[i] < 0x20 ||
            (unsigned char) password[i] > 0x7e) {
            send_http("PASSWORD_INVALID", 16, exit, &err);
            goto exit;
        }
    }
    memcpy(user_config->web_password, password, length + 1);
    err = mico_system_context_update(sys_config);
    require_noerr(err, exit);
    err = httpd_auth_init(user_config->web_password[0] ? "admin" : "",
                          user_config->web_password);
    require_noerr(err, exit);
    send_http("OK", 2, exit, &err);

exit:
    memset(password, 0, sizeof(password));
    return err;
}

const struct httpd_wsgi_call g_app_handlers[] = {
        {"/",                 HTTPD_HDR_DEFORT, 0,                             HttpGetIndexPage, NULL,                       NULL, NULL},
        {"/assets", HTTPD_HDR_ADD_SERVER |
                    HTTPD_HDR_ADD_CONN_CLOSE,   APP_HTTP_FLAGS_NO_EXACT_MATCH, HttpGetAssets,    NULL,                       NULL, NULL},
        {"/socket",           HTTPD_HDR_DEFORT, 0, NULL,                                              HttpSetSocketStatus,   NULL, NULL},
        {"/status",           HTTPD_HDR_DEFORT, 0,                             HttpGetTc1Status, NULL,                       NULL, NULL},
        {"/power",            HTTPD_HDR_DEFORT, 0,                             HttpGetPowerInfo,      HttpGetPowerInfo,      NULL, NULL},
        {"/wifi/config",      HTTPD_HDR_DEFORT, 0,                             HttpGetWifiConfig,     HttpSetWifiConfig,     NULL, NULL},
        {"/wifi/scan",        HTTPD_HDR_DEFORT, 0,                             HttpGetWifiScan,       HttpSetWifiScan,       NULL, NULL},
        {"/mqtt/config",      HTTPD_HDR_DEFORT, 0, NULL,                                              HttpSetMqttConfig,     NULL, NULL},
        {"/reboot",           HTTPD_HDR_DEFORT, 0, NULL,                                              HttpSetRebootSystem,   NULL, NULL},
        {"/mqtt/report/freq", HTTPD_HDR_DEFORT, 0,                             HttpGetMqttReportFreq, HttpSetMqttReportFreq, NULL, NULL},
        {"/log",              HTTPD_HDR_DEFORT, 0,                             HttpGetLog,       NULL,                       NULL, NULL},
        {"/task",             HTTPD_HDR_DEFORT, APP_HTTP_FLAGS_NO_EXACT_MATCH, HttpGetTasks,          HttpAddTask,           NULL, HttpDelTask},
        {"/ota/status", HTTPD_HDR_DEFORT, 0, OtaDetailedStatus, NULL, NULL, NULL},
        {"/ota",              HTTPD_HDR_DEFORT, 0,                             Otastatus,             OtaStart,              NULL, NULL},
        {"/led",              HTTPD_HDR_DEFORT, 0,                             LedStatus,             LedSetEnabled,         NULL, NULL},
        {"/socketAll",        HTTPD_HDR_DEFORT, 0, NULL,                                              TotalSocketSetEnabled, NULL, NULL},
        {"/socketNames",      HTTPD_HDR_DEFORT, 0, NULL,                                              HttpSetSocketName,     NULL, NULL},
        {"/childLock",        HTTPD_HDR_DEFORT, 0, NULL,                                              HttpSetChildLock,      NULL, NULL},
        {"/deviceName",       HTTPD_HDR_DEFORT, 0, NULL,                                              HttpSetDeviceName,     NULL, NULL},
        {"/buttonEvents",     HTTPD_HDR_DEFORT, 0,                             HttpGetButtonEvents,   HttpSetButtonEvent,    NULL, NULL},
        {"/ota/fileUploadUnsigned", HTTPD_HDR_DEFORT, 0, NULL, HttpSetOTAUnsigned, NULL, NULL},
        {"/ota/fileUpload",     HTTPD_HDR_DEFORT, 0,                             NULL,   HttpSetOTAFile,    NULL, NULL},
        {"/power/coefficient", HTTPD_HDR_DEFORT, 0, NULL,                                              HttpSetPowerCoefficient, NULL, NULL},
        {"/auth/password",    HTTPD_HDR_DEFORT, 0,                             NULL,   HttpSetWebPassword, NULL, NULL},
};

static int g_app_handlers_no = sizeof(g_app_handlers) / sizeof(struct httpd_wsgi_call);

static void AppHttpRegisterHandlers() {
    int rc;
    rc = httpd_register_wsgi_handlers((struct httpd_wsgi_call *) g_app_handlers, g_app_handlers_no);
    if (rc) { http_log("failed to register test web handler");
    }
}

static int _AppHttpdStart() {
    OSStatus err = kNoErr;http_log("initializing web-services");

    /*Initialize HTTPD*/
    if (is_http_init == false) {
        err = httpd_init();
        require_noerr_action(err, exit, http_log("failed to initialize httpd"));
        is_http_init = true;
    }

    /*Start http thread*/
    err = httpd_start();
    if (err != kNoErr) { http_log("failed to start httpd thread");
        httpd_shutdown();
    }
    exit:
    return err;
}

int AppHttpdStart(void) {
    OSStatus err = kNoErr;

    err = _AppHttpdStart();
    require_noerr(err, exit);

    if (is_handlers_registered == false) {
        AppHttpRegisterHandlers();
        is_handlers_registered = true;
    }

    exit:
    return err;
}

int AppHttpdStop() {
    OSStatus err = kNoErr;

    /* HTTPD and services */
    http_log("stopping down httpd");
    err = httpd_stop();
    require_noerr_action(err, exit, http_log("failed to halt httpd"));

    exit:
    return err;
}

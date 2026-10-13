#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define MQTT_USER_MAX 63u
#define MQTT_PASSWORD_MAX 256u
#define MQTT_TOPIC_MAX 512u
#define MQTT_KEY_MAX (2u + MQTT_USER_MAX * 2u + 1u)
enum { MQTT_TOPIC_LEGACY = 0, MQTT_TOPIC_ACCOUNT = 1 };
enum { MQTT_DISCONNECTED, MQTT_CONNECTING, MQTT_CONFIG_ERROR,
       MQTT_AUTH_FAILED, MQTT_WAITING_ACL, MQTT_READY };
typedef struct {
    uint8_t mode;
    char model[4], mac[13], upper_mac[13], key[MQTT_KEY_MAX], base[192];
} mqtt_account_t;

static inline bool mqtt_format(char *out, size_t size, const char *fmt, ...)
{
    va_list args; va_start(args, fmt);
    int n = vsnprintf(out, size, fmt, args); va_end(args);
    if (n < 0 || (size_t)n >= size) { if (size) out[0] = 0; return false; }
    return true;
}
static inline bool mqtt_hex(char *out, size_t size, const char *value)
{
    static const char hex[] = "0123456789abcdef";
    size_t n = strlen(value);
    if (!size || n > (size - 1) / 2) { if (size) out[0] = 0; return false; }
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)value[i];
        out[2*i] = hex[c >> 4]; out[2*i+1] = hex[c & 15];
    }
    out[2*n] = 0; return true;
}
static inline bool mqtt_account_init(mqtt_account_t *a, unsigned mode,
                                    const char *model, const char *user, const char *mac)
{
    memset(a, 0, sizeof(*a));
    if (mode > MQTT_TOPIC_ACCOUNT || strlen(user) > MQTT_USER_MAX ||
        (mode == MQTT_TOPIC_ACCOUNT && !*user) || strlen(mac) != 12 ||
        (strcmp(model,"m1") && strcmp(model,"a1") && strcmp(model,"tc1"))) return false;
    a->mode = mode; strcpy(a->model, model);
    for (unsigned i = 0; i < 12; ++i) {
        char c = mac[i];
        if (c >= 'A' && c <= 'F') c += 'a' - 'A';
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        a->mac[i] = c; a->upper_mac[i] = c >= 'a' ? c - ('a'-'A') : c;
    }
    if (mode == MQTT_TOPIC_ACCOUNT) {
        strcpy(a->key, "u_");
        if (!mqtt_hex(a->key+2, sizeof(a->key)-2, user)) return false;
        return mqtt_format(a->base, sizeof(a->base), "phicomm/%s/%s/%s", model, a->key, a->mac);
    }
    return mqtt_format(a->base, sizeof(a->base), "phicomm/%s/%s", model, a->mac);
}
static inline bool mqtt_leaf(const mqtt_account_t *a, const char *leaf, char *out, size_t n)
{ return mqtt_format(out,n,"%s/%s",a->base,leaf); }
static inline bool mqtt_identity(const mqtt_account_t *a, const char *entity, char *out, size_t n)
{
    const char *mac = !strcmp(a->model,"tc1") ? a->upper_mac : a->mac;
    if (a->mode) return mqtt_format(out,n,"%s_%s_%s%s%s",a->key,a->model,a->mac,*entity?"_":"",entity);
    if (!strcmp(a->model,"tc1")) return mqtt_format(out,n,"%s%s%s",mac,*entity?"_":"",entity);
    return mqtt_format(out,n,"%s_%s%s%s",a->model,mac,*entity?"_":"",entity);
}
static inline bool mqtt_ha(const mqtt_account_t *a, const char *component,
                          const char *entity, char *out, size_t n)
{
    if (a->mode) return mqtt_format(out,n,"homeassistant/%s/%s/%s_%s_%s/config",component,a->key,a->model,a->mac,entity);
    if (!strcmp(a->model,"tc1")) return mqtt_format(out,n,"homeassistant/%s/%s/%s/config",component,a->upper_mac,entity);
    return mqtt_format(out,n,"homeassistant/%s/%s_%s/%s/config",component,a->model,a->mac,entity);
}
static inline bool mqtt_aux(const mqtt_account_t *a, const char *kind,
                           const char *id, char *out, size_t n)
{
    if (a->mode) return mqtt_format(out,n,"phicomm/%s/%s/%s",kind,a->key,id);
    return mqtt_format(out,n,"phicomm/%s/%s",kind,id);
}
static inline bool mqtt_linkage(const mqtt_account_t *a, const char *root, char *out, size_t n)
{
    if (a->mode) return mqtt_format(out,n,"phicomm/linkage/%s/%s",a->key,root);
    return mqtt_format(out,n,"%s",root);
}
static inline const char *mqtt_announcement(const mqtt_account_t *a)
{
    if (!strcmp(a->model,"tc1")) return "{\"schema\":1,\"model\":\"tc1\",\"version\":\"v2\"}";
    if (!strcmp(a->model,"m1")) return "{\"schema\":1,\"model\":\"m1\",\"version\":\"community\"}";
    return "{\"schema\":1,\"model\":\"a1\",\"version\":\"community\"}";
}
static inline const char *mqtt_status_name(unsigned state)
{
    static const char *names[] = {"disconnected","connecting","config_error","auth_failed","waiting_acl","ready"};
    return state < sizeof(names)/sizeof(names[0]) ? names[state] : names[0];
}
static inline unsigned mqtt_retry_seconds(unsigned attempt)
{
    static const unsigned seconds[] = {1,2,4,8,15,30};
    return seconds[attempt < 5 ? attempt : 5];
}

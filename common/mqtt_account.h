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
/* TMCP = Tom MQTT Cloud Protocol; the v2 topic layout. LEGACY is the
 * pre-TMCP topic layout, still readable from old configuration. */
enum { MQTT_TOPIC_LEGACY = 0, MQTT_TOPIC_TMCP = 1 };
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
    if (strlen(user) > MQTT_USER_MAX || strlen(mac) != 12 ||
        (strcmp(model,"m1") && strcmp(model,"a1") && strcmp(model,"tc1"))) return false;
    if (mode > MQTT_TOPIC_TMCP) return false;
    if (strcmp(model,"tc1")) mode = MQTT_TOPIC_TMCP;
    a->mode = mode; strcpy(a->model, model);
    for (unsigned i = 0; i < 12; ++i) {
        char c = mac[i];
        if (c >= 'A' && c <= 'F') c += 'a' - 'A';
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        a->mac[i] = c; a->upper_mac[i] = c >= 'a' ? c - ('a'-'A') : c;
    }
    if (*user) {
        strcpy(a->key,"u_");if(!mqtt_hex(a->key+2,sizeof(a->key)-2,user))return false;
    } else strcpy(a->key,"local");
    if (!mode) return mqtt_format(a->base,sizeof(a->base),"device/ztc1/%s",a->upper_mac);
    return mqtt_format(a->base,sizeof(a->base),"phicomm/v2/%s/%s/%s",a->key,model,a->mac);
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
    if (a->mode) return mqtt_format(out,n,"phicomm/v2/%s/%s/%s",a->key,kind,id);
    return mqtt_format(out,n,"phicomm/%s/%s",kind,id);
}
static inline bool mqtt_linkage(const mqtt_account_t *a, const char *root, char *out, size_t n)
{
    if (a->mode) return mqtt_format(out,n,"phicomm/linkage/%s/%s",a->key,root);
    return mqtt_format(out,n,"%s",root);
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

static inline bool mqtt_link_topic(const mqtt_account_t *a,const char *model,const char *mac,const char *peer,const char *leaf,char *out,size_t n) {
 return mqtt_format(out,n,"phicomm/v2/%s/%s/%s/link/%s/%s",a->key,model,mac,peer,leaf);
}

/* A queued reply owns its original destination, without the derived base and
 * uppercase MAC copies in mqtt_account_t. This also permits mixed sessions. */
typedef struct {
    char key[MQTT_KEY_MAX], model[4], mac[13];
    uint8_t mode;
} mqtt_target_t;
static inline void mqtt_target_set(mqtt_target_t *target, const mqtt_account_t *a)
{
    strcpy(target->key,a->key);strcpy(target->model,a->model);
    strcpy(target->mac,a->mac);target->mode=a->mode;
}
static inline bool mqtt_target_leaf(const mqtt_target_t *target, const char *leaf,
                                    char *out, size_t n)
{
    if (target->mode)
        return mqtt_format(out,n,"phicomm/v2/%s/%s/%s/%s",
                           target->key,target->model,target->mac,leaf);
    char upper[13];
    for(unsigned i=0;i<13;i++) {
        char c=target->mac[i];upper[i]=c>='a'&&c<='f'?c-('a'-'A'):c;
    }
    return mqtt_format(out,n,"device/ztc1/%s/%s",upper,leaf);
}

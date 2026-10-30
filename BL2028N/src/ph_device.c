/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "../protocol/diagnostics_ids.h"
bool ph_binary_format(ph_wire_format f) { return f == PH_WIRE_MSGPACK || f == PH_WIRE_CBOR; }
void ph_settings_default(ph_settings *s) {
    memset(s, 0, sizeof(*s)); s->magic = PH_STORE_MAGIC; s->indicator = 1; s->report_seconds=30; strcpy(s->name,"S30-V1B"); strcpy(s->socket_name,"S30-V1B");
    s->coefficients[0]=PH_METER_DEFAULT_VOLTAGE_COEFFICIENT;
    s->coefficients[1]=PH_METER_DEFAULT_CURRENT_COEFFICIENT;
    s->coefficients[2]=PH_METER_DEFAULT_POWER_COEFFICIENT;
    ph_auth_store_empty(&s->auth);
}
bool ph_settings_valid(const ph_settings *s) {
    return s && s->magic == PH_STORE_MAGIC && s->revision <= INT32_MAX &&
        s->report_seconds>=1 && s->report_seconds<=3600 && memchr(s->name,0,sizeof(s->name)) && memchr(s->socket_name,0,sizeof(s->socket_name)) &&
        s->child_lock <= 1 && s->indicator <= 1 && s->power_on <= 1 &&
        s->calibrated <= 1 && isfinite(s->energy_kwh) && s->energy_kwh>=0 && s->energy_kwh<=1e9 &&
        isfinite(s->coefficients[0]) && s->coefficients[0]>=0.001 && s->coefficients[0]<=100 &&
        isfinite(s->coefficients[1]) && s->coefficients[1]>=0.001 && s->coefficients[1]<=100 &&
        isfinite(s->coefficients[2]) && s->coefficients[2]>=0.001 && s->coefficients[2]<=100 && ph_auth_store_valid(&s->auth);
}
bool ph_settings_migrate_meter_defaults(ph_settings *s) {
    /* Only the exact, never-edited community defaults are eligible. Any
     * explicit calibration, even one equal to the old defaults, wins. */
    if (!ph_settings_valid(s) || s->calibrated || s->coefficients[0]!=1.88 ||
        s->coefficients[1]!=1.0 || s->coefficients[2]!=1.88) return false;
    s->coefficients[0]=PH_METER_DEFAULT_VOLTAGE_COEFFICIENT;
    s->coefficients[1]=PH_METER_DEFAULT_CURRENT_COEFFICIENT;
    s->coefficients[2]=PH_METER_DEFAULT_POWER_COEFFICIENT;
    return true;
}
static bool mac_valid(const char *s) {
    if (!s || strlen(s) != 12) return false;
    for (unsigned i = 0; i < 12; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}
bool ph_topic_root(const char *username, const char *mac, char *out, size_t n) {
    char scope[131] = "local";
    if (!username || !mac_valid(mac) || strlen(username) > 64) return false;
    if (*username) {
        static const char h[] = "0123456789abcdef"; size_t i;
        scope[0] = 'u'; scope[1] = '_';
        for (i = 0; username[i]; ++i) { unsigned c = (unsigned char)username[i]; scope[2+i*2] = h[c>>4]; scope[3+i*2] = h[c&15]; }
        scope[2+i*2] = 0;
    }
    int used = snprintf(out, n, "phicomm/v2/%s/" PH_MODEL "/%s", scope, mac);
    return used > 0 && (size_t)used < n;
}
bool ph_device_init(ph_device *d, const ph_device_ops *ops, const ph_settings *s,
                    const char *mac, const char *version, uint32_t boot) {
    if (!d || !ops || !ops->now || !ops->save || !ops->relay || !mac_valid(mac) || !version || strlen(version) >= sizeof(d->version) || !boot) return false;
    memset(d, 0, sizeof(*d)); d->ops = *ops;
    if (s && ph_settings_valid(s)) d->settings = *s; else ph_settings_default(&d->settings);
    strcpy(d->mac, mac); strcpy(d->version, version); d->boot = boot;
    ph_meter_init(&d->meter,d->settings.energy_kwh);
    d->relay = d->settings.power_on; d->ops.relay(d->ops.user, d->relay); return true;
}
static void identity(ph_device *d, ph_writer *w) {
    phw_text(w, "name", d->settings.name);
    phw_text(w, "model", PH_MODEL); phw_text(w, "hardware", "ORVIBO S30-V1B / BL2028N");
    phw_text(w, "model_name", "S30-V1B");phw_text(w, "manufacturer", "ORVIBO");
    phw_text(w, "project", "iotcloud");phw_text(w, "chip", "BK7231N");phw_text(w, "module", "BL2028N");
    phw_text(w, "device_id", d->mac); phw_text(w, "mac", d->mac);
    phw_text(w, "firmware_version", d->version); phw_text(w, "version", d->version);
    phw_text(w, "protocol", "openiot");phw_text(w, "command_protocol", "tmcp"); phw_u32(w, "protocol_version", 2);
    phw_u32(w,"api_version",1);phw_u32(w,"wire_version",1);
    phw_text(w,"variant","full");phw_text(w,"default_wire_format","msgpack");
    phw_begin(w,"wire_formats",true);phw_text(w,NULL,"msgpack");phw_text(w,NULL,"cbor");phw_end(w);
}
static void status_build(ph_device *d, ph_writer *w) {
    identity(d, w);
    phw_u32(w, "boot", d->boot); phw_u32(w, "sequence", d->sequence);
    phw_u32(w, "revision", d->settings.revision); phw_bool(w, "power", d->relay);
    phw_begin(w, "socket_states", true); phw_bool(w, NULL, d->relay); phw_end(w);
    phw_begin(w, "socket_names", true); phw_text(w, NULL, d->settings.socket_name); phw_end(w);
    phw_bool(w,"led_enabled",d->settings.indicator); phw_u32(w,"report_seconds",d->settings.report_seconds);
    phw_bool(w,"power_on",d->settings.power_on);
    phw_number(w,"voltage_coefficient",d->settings.coefficients[0]);
    phw_number(w,"current_coefficient",d->settings.coefficients[1]);
    phw_number(w,"power_coefficient",d->settings.coefficients[2]);
    phw_bool(w, "child_lock", d->settings.child_lock); phw_bool(w, "indicator", d->settings.indicator);
    phw_bool(w, "power_metering_supported", true);
    ph_meter_write(&d->meter,w,d->ops.now(d->ops.user),d->settings.calibrated);
    if (d->ops.network_info) d->ops.network_info(d->ops.user, w);
}
void ph_status_cache_release(ph_device *d) {
    free(d->status_cache);d->status_cache=NULL;d->status_size=0;
}
void ph_device_set_maintenance(ph_device *d,bool enabled) {
    d->maintenance=enabled;
    if(enabled)ph_status_cache_release(d);
}
void ph_status(ph_device *d,ph_writer *w) {
    unsigned fields=ph_meter_fields(&d->meter,d->ops.now(d->ops.user));
    uint32_t network=d->ops.network_stamp?d->ops.network_stamp(d->ops.user):0;
    /* OTA reclaims this optional cache. Polling must not allocate it again
     * while the downloader needs its stack, receive and flash buffers. */
    if(d->maintenance||(d->ops.network_info&&!d->ops.network_stamp)){status_build(d,w);return;}
    if(!d->status_cache)d->status_cache=malloc(PH_STATUS_CACHE_MAX);
    if(!d->status_cache){status_build(d,w);return;}
    if(!d->status_size||d->cache_sequence!=d->sequence||d->cache_frames!=d->meter.frames||d->cache_fields!=fields||d->cache_network!=network){
        ph_writer cached;struct printbuf buffer;
        phw_init_fixed(&cached,PH_WIRE_MSGPACK,&buffer,d->status_cache,PH_STATUS_CACHE_MAX);
        phw_begin(&cached,NULL,false);status_build(d,&cached);phw_end(&cached);
        d->status_size=phw_finish_fixed(&cached);
        d->cache_sequence=d->sequence;d->cache_frames=d->meter.frames;d->cache_fields=fields;d->cache_network=network;++d->cache_builds;
        if(!d->status_size){ph_status_cache_release(d);status_build(d,w);return;}
    }
    phw_cached_fields(w,d->status_cache,d->status_size);
}
void ph_diag_uint(ph_writer *w,unsigned id,uint32_t value){phw_begin(w,NULL,true);phw_u32(w,NULL,id);phw_u32(w,NULL,value);phw_end(w);}
void ph_diag_int(ph_writer *w,unsigned id,int32_t value){phw_begin(w,NULL,true);phw_u32(w,NULL,id);phw_i32(w,NULL,value);phw_end(w);}
void ph_diagnostics(ph_device *d,ph_writer *w) {
    phw_u32(w,"schema",PH_DIAGNOSTICS_SCHEMA);phw_begin(w,"fields",true);
    ph_diag_uint(w,PH_DIAG_UPTIME_MS,d->ops.now(d->ops.user));
    ph_diag_uint(w,PH_DIAG_UPTIME_SECONDS,d->ops.now(d->ops.user)/1000);
    ph_diag_uint(w,PH_DIAG_METERING_RECEIVED_BYTES,d->meter.received_bytes);
    ph_diag_uint(w,PH_DIAG_METERING_FRAMES,d->meter.frames);
    ph_diag_uint(w,PH_DIAG_METERING_BAD_FRAMES,d->meter.bad_frames);
    ph_diag_uint(w,PH_DIAG_METERING_DISCONTINUITIES,d->meter.discontinuities);
    ph_diag_uint(w,PH_DIAG_STATUS_CACHE_BUILDS,d->cache_builds);
    if(d->ops.diagnostics)d->ops.diagnostics(d->ops.user,w);
    phw_end(w);
}
static const char *actions[] = {"info.get", "status.get", "capabilities.get", "diagnostics.get", "config.get", "config.set", "power.get", "outlet.set", "indicator.set", "child_lock.set", "system.reboot", "system.reset", "wifi.scan", "wifi.scan.get", "ota.start"};
void ph_capabilities(ph_device *d,ph_writer *w) {
    identity(d,w);phw_text(w, "mqtt_protocol", "openiot");phw_u32(w, "mqtt_protocol_version", 2);
    phw_begin(w, "actions", true);
    for (unsigned i = 0; i < sizeof(actions)/sizeof(*actions); ++i) phw_text(w, NULL, actions[i]);
    phw_end(w); phw_u32(w, "outlet_count", 1); phw_u32(w, "udp_port", 11288);
    phw_bool(w, "local_auth", true); phw_bool(w, "power_metering_supported", true);
    phw_text(w,"metering_chip","HLW8032");
    phw_bool(w,"ota_upload",false); phw_bool(w,"config_backup",false); phw_bool(w,"config_recovery",false);
    phw_u32(w,"mqtt_password_max",127);
    phw_u32(w,"ota_url_max",127);phw_bool(w,"wifi_trial",false);
    phw_begin(w,"status_views",true);phw_text(w,NULL,"full");phw_text(w,NULL,"live");phw_end(w);
}
static void config(ph_device *d, ph_writer *w) {
    ph_status(d,w);
}
size_t ph_query(ph_device *d, const char *action, ph_wire_format f, void *out, size_t n) {
    ph_writer w; struct printbuf b;
    if (!ph_binary_format(f) || !phw_init_fixed(&w, f, &b, out, n)) return 0;
    phw_begin(&w, NULL, false);
    if (!strcmp(action, "status.get")) ph_status(d, &w);
    else if (!strcmp(action, "info.get")) identity(d, &w);
    else if (!strcmp(action, "capabilities.get")) ph_capabilities(d,&w);
    else if (!strcmp(action, "diagnostics.get")) ph_diagnostics(d,&w);
    else if (!strcmp(action, "config.get")) config(d, &w);
    else return 0;
    phw_end(&w); return phw_finish_fixed(&w);
}
static bool text_field(const ph_value *v, const char *k, char *out, size_t n) {
    ph_value t; return phv_get(v, k, &t) && phv_text(&t, out, n);
}
static bool boolean(const ph_value *v, const char *k, bool *out) {
    ph_value t; return phv_get(v, k, &t) && phv_bool(&t, out);
}
static bool identifier(const char *s) {
    if (!*s) return false;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '-' || *s == '.')) return false;
    return true;
}
/* Sort nested parameter maps so equivalent retry
 * requests compare identically across map order and binary formats. */
static size_t canonical_params(const ph_value *p, void *out, size_t size) {
    ph_writer w;struct printbuf b;char previous[128]="";
    phw_init_fixed(&w,PH_WIRE_MSGPACK,&b,out,size);phw_begin(&w,NULL,false);
    for(unsigned i=0;i<p->count;++i){
        ph_iterator it;ph_value v,chosen;char key[128],best[128]="";bool found=false;
        if(!phv_iter(p,&it))return 0;
        while(phv_next(&it,key,&v))if((!i||strcmp(key,previous)>0)&&(!found||strcmp(key,best)<0)){strcpy(best,key);chosen=v;found=true;}
        if(!found)return 0;
        phw_value_canonical(&w,best,&chosen);strcpy(previous,best);
    }
    phw_end(&w);return phw_finish_fixed(&w);
}
static const char *execute(ph_device *d, const char *action, const ph_value *p, ph_writer *w, bool admin) {
    bool on; ph_value v;
    if (!strcmp(action, "status.get")) {
        if (!phv_keys(p, "|view|")) return "invalid_params";
        if (phv_get(p, "view", &v) && !phv_equal_text(&v, "full") && !phv_equal_text(&v, "live")) return "invalid_params";
        ph_status(d, w); return "ok";
    }
    if (!strcmp(action, "capabilities.get") || !strcmp(action,"diagnostics.get") || !strcmp(action, "info.get") || !strcmp(action, "config.get") || !strcmp(action, "power.get")) {
        if (p->count) return "invalid_params";
        if (!strcmp(action, "capabilities.get")) ph_capabilities(d,w);
        else if (!strcmp(action,"diagnostics.get")) ph_diagnostics(d,w);
        else if (!strcmp(action, "info.get")) identity(d, w);
        else if (!strcmp(action, "config.get")) config(d, w);
        else {
            phw_bool(w,"on",d->relay);phw_bool(w,"metering_supported",true);
            if(ph_meter_fields(&d->meter,d->ops.now(d->ops.user))&4)phw_number(w,"power_watts",d->meter.value[2]);else phw_null(w,"power_watts");
            phw_number(w,"energy_kwh",d->meter.energy_kwh);ph_meter_write(&d->meter,w,d->ops.now(d->ops.user),d->settings.calibrated);
        }
        return "ok";
    }
    if (!strcmp(action, "outlet.set")) {
        int64_t index;
        if (!phv_keys(p, "|index||on|") || !boolean(p, "on", &on) ||
            (phv_get(p, "index", &v) && (!phv_int(&v, &index) || index != 0))) return "invalid_params";
        if (d->local_hold && (uint32_t)(d->ops.now(d->ops.user)-d->local_at) < 1500u) return "busy";
        d->relay = on; ++d->sequence; d->ops.relay(d->ops.user, on); phw_bool(w, "on", on); return "accepted";
    }
    if (!strcmp(action,"wifi.scan.get"))return d->ops.platform_action?d->ops.platform_action(d->ops.user,action,p,w,admin):"unsupported";
    if (!admin) return "unauthorized";
    if (!strcmp(action, "system.reboot")) {
        if (p->count) return "invalid_params";
        if (!d->ops.reboot) return "unsupported";
        d->ops.reboot(d->ops.user); return "accepted";
    }
    ph_settings next = d->settings;
    if (!strcmp(action, "indicator.set") || !strcmp(action, "child_lock.set")) {
        if (!phv_keys(p, "|enabled|") || !boolean(p, "enabled", &on)) return "invalid_params";
        if (!strcmp(action, "indicator.set")) next.indicator = on; else next.child_lock = on;
    } else if (!strcmp(action, "config.set")) {
        char section[24];ph_value values;
        if(p->count!=2 || !phv_keys(p,"|section||values|") ||
           !text_field(p,"section",section,sizeof(section)) || !phv_get(p,"values",&values) || values.kind!=PHV_MAP)return "invalid_params";
        if(!strcmp(section,"device")) {
            if(!values.count || !phv_keys(&values,"|name||power_on|"))return "invalid_params";
            if(phv_get(&values,"name",&v)) {
                if(!phv_text(&v,next.name,sizeof(next.name)))return "invalid_params";
                if(!*next.name)strcpy(next.name,"S30-V1B");
            }
            if(phv_get(&values,"power_on",&v)){if(!phv_bool(&v,&on))return "invalid_params";next.power_on=on;}
        } else if(!strcmp(section,"socket")) {
            int64_t index;
            if(values.count!=2 || !phv_keys(&values,"|index||name|") ||
                !phv_get(&values,"index",&v)||!phv_int(&v,&index)||index!=0 ||
                !text_field(&values,"name",next.socket_name,sizeof(next.socket_name))||!*next.socket_name)return "invalid_params";
        } else if(!strcmp(section,"report")) {
            int64_t seconds;
            if(values.count!=1||!phv_get(&values,"seconds",&v)||!phv_int(&v,&seconds)||seconds<1||seconds>3600)return "invalid_params";
            next.report_seconds=(uint32_t)seconds;
        } else if(!strcmp(section,"meter")) {
            const char *coeff[]={"voltage_coefficient","current_coefficient","power_coefficient"};
            if(!values.count||!phv_keys(&values,"|voltage_coefficient||current_coefficient||power_coefficient|"))return "invalid_params";
            for(unsigned i=0;i<3;++i)if(phv_get(&values,coeff[i],&v)) {
                double c;if(!phv_number(&v,&c)||!isfinite(c)||c<0.001||c>100)return "invalid_params";
                next.coefficients[i]=c;next.calibrated=1;
            }
        } else return d->ops.platform_action?d->ops.platform_action(d->ops.user,action,p,w,admin):"unsupported";
    } else return d->ops.platform_action?d->ops.platform_action(d->ops.user,action,p,w,admin):"unsupported";
    if (next.revision == INT32_MAX) return "busy";
    ++next.revision;
    next.energy_kwh=d->meter.energy_kwh;
    if (!d->ops.save(d->ops.user, &next)) return "storage_error";
    d->settings = next;
    /* Recompute fresh samples immediately when calibration changes. */
    for(unsigned i=0;i<3;++i){d->meter.value[i]=d->meter.raw[i]*next.coefficients[i];if(d->meter.value[i]>(i==0?500:i==1?100:25000))d->meter.fields&=~(1u<<i);}
    ++d->sequence; phw_u32(w,"revision",next.revision); return "accepted";
}
/* Fixed response storage and an eight-command idempotency window are reserved
 * before mutations. Canonical params also catch conflicts across wire formats. */
size_t ph_command(ph_device *d, const void *bytes, size_t n, ph_wire_format input,
                  ph_wire_format output, bool authorized, bool admin, void *out, size_t capacity) {
    ph_value root, params; char id[65] = "", action[40] = "";
    unsigned char data[PH_OUTPUT_MAX], canonical[512]; size_t data_n = 0, canonical_n = 0;
    ph_writer w; struct printbuf b; const char *code = "invalid_params";
    /* Guarantee response capacity before changing hardware or Flash. */
    if (!ph_binary_format(input) || !ph_binary_format(output) || capacity < PH_OUTPUT_MAX || n > PH_INPUT_MAX) return 0;
    if (!phv_parse(input, bytes, n, &root) || root.kind != PHV_MAP) goto response;
    if (!phv_keys(&root, "|request_id||action||params|") || (phv_get(&root,"request_id",&params) && (!text_field(&root,"request_id",id,sizeof(id)) || !identifier(id))) || !text_field(&root, "action", action, sizeof(action)) || !phv_get(&root, "params", &params) || params.kind != PHV_MAP) goto response;
    if (!authorized) { code = "unauthorized"; goto response; }
    canonical_n=canonical_params(&params,canonical,sizeof(canonical));
    if (!canonical_n) goto response;
    size_t action_len=strlen(action);
    bool mutation = action_len<4 || strcmp(action+action_len-4,".get") != 0;
    if (mutation && !identifier(id))goto response;
    if (mutation) for (unsigned i = 0; i < 8; ++i) {
        ph_receipt *r = &d->receipts[i]; if (strcmp(id, r->id)) continue;
        if (strcmp(action, r->action) || canonical_n != r->params_len || memcmp(canonical, r->params, canonical_n)) { code = "conflict"; goto response; }
        code = r->code; memcpy(data, r->data, r->data_len); data_n = r->data_len; goto response;
    }
    if(mutation && d->maintenance){code="busy";goto response;}
    phw_init_fixed(&w, PH_WIRE_MSGPACK, &b, data, mutation ? sizeof(d->receipts[0].data) : sizeof(data)); phw_begin(&w, NULL, false);
    code = execute(d, action, &params, &w, admin); phw_end(&w); data_n = phw_finish_fixed(&w);
    if (!data_n) return 0;
    if(mutation&&!strcmp(code,"accepted"))d->status_size=0;
    if (mutation && strcmp(code, "busy") && strcmp(code, "unauthorized")) {
        ph_receipt *r = &d->receipts[d->next_receipt++ % 8];
        strcpy(r->id, id); strcpy(r->action, action); memcpy(r->params, canonical, canonical_n); r->params_len = canonical_n;
        memcpy(r->data, data, data_n); r->data_len = data_n; r->code = code;
    }
response:
    phw_init_fixed(&w, output, &b, out, capacity); phw_begin(&w, NULL, false);
    if (identifier(id)) phw_text(&w, "request_id", id);
    phw_text(&w, "code", code); phw_text(&w, "message", code);
    phw_begin(&w, "data", false);
    if (data_n) phw_cached_fields(&w, data, data_n);
    phw_end(&w);
    phw_end(&w); return phw_finish_fixed(&w);
}
bool ph_local_toggle(ph_device *d) {
    if (d->settings.child_lock || d->maintenance) return false;
    d->relay = !d->relay; d->local_at = d->ops.now(d->ops.user); d->local_hold = true;
    ++d->sequence; d->ops.relay(d->ops.user, d->relay); return true;
}

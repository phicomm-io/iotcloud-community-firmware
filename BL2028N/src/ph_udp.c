/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "udp.h"
#include "../protocol/udp/packet.h"
#include <string.h>
static bool text(const ph_value *p, const char *key, char *s, size_t n) {
    ph_value v; return phv_get(p, key, &v) && phv_text(&v, s, n);
}
static bool number(const ph_value *p, const char *key, uint32_t *n) {
    ph_value v; int64_t i;
    if (!phv_get(p,key,&v) || !phv_int(&v,&i) || i < 0 || (uint64_t)i > UINT32_MAX) return false;
    *n = (uint32_t)i; return true;
}
static bool peer_ok(const char *s) {
    if (strlen(s) != 12) return false;
    for (unsigned i=0;i<12;++i) if (!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f'))) return false;
    return true;
}
size_t ph_udp_handle(ph_udp_state *u, ph_device *d, ph_auth_state *auth, const void *bytes, size_t n, void *out, size_t cap) {
    ph_value v, body, value; ph_wire_format f = ph_wire_detect(bytes,n);
    char src[13], dst[16], op[32], token[33] = "", secret[33] = "", peer[13];
    uint32_t version, boot, id; const char *reply = NULL, *error = NULL;
    unsigned char data[PH_OUTPUT_MAX]; size_t data_n = 0;
    feature_packet_t packet;
    if (n > PH_INPUT_MAX || cap < PH_OUTPUT_MAX || !ph_binary_format(f) ||
        !phv_parse(f,bytes,n,&v) || !packet_parse(v,&packet) ||
        !phv_keys(&v,"|v||src||dst||boot||id||op||token||body||group||domain||kind|") ||
        !number(&v,"v",&version) || version != 2 || !number(&v,"boot",&boot) || !boot ||
        !number(&v,"id",&id) || !id || !text(&v,"src",src,sizeof(src)) || !peer_ok(src) ||
        !strcmp(src,d->mac) || !text(&v,"dst",dst,sizeof(dst)) ||
        !text(&v,"op",op,sizeof(op)) || !text(&v,"token",token,sizeof(token)) || !phv_get(&v,"body",&body) || body.kind != PHV_MAP) return 0;
    const char *optional[]={"group","domain","kind"};const size_t limits[]={33,16,16};char extra[33];
    for(unsigned i=0;i<3;++i)if(phv_get(&v,optional[i],&value)&&!phv_text(&value,extra,limits[i]))return 0;
    bool broadcast = !strcmp(dst,"broadcast");
    if (strcmp(dst,d->mac) && !broadcast) return 0;
    ph_writer w; struct printbuf b;
    if (!strcmp(op,"discovery")) {
        if (body.count) return 0;
        reply = "discovery.response";
        data_n = ph_query(d,"info.get",f,data,sizeof(data));
    } else {
        if (broadcast) return 0; /* All authentication and control must be unicast. */
        if (!strcmp(op,"auth.pair")) {
            char password[33]; reply = "auth.paired";
            uint32_t scopes=7;
            if (!phv_keys(&body,"|peer||password||scopes|") || !text(&body,"peer",peer,sizeof(peer)) || strcmp(peer,src) || !text(&body,"password",password,sizeof(password)) || (phv_get(&body,"scopes",&value)&&(!number(&body,"scopes",&scopes)||!scopes||(scopes&~7u)))) return 0;
            if (!ph_auth_pair(auth,password,src,scopes,secret)) error="unauthorized";
            memset(password,0,sizeof(password));
        } else if (!strcmp(op,"auth.resume")) {
            char key[33]; reply = "auth.resumed";
            if (!phv_keys(&body,"|peer||key|") || !text(&body,"peer",peer,sizeof(peer)) || strcmp(peer,src) || !text(&body,"key",key,sizeof(key))) return 0;
            if (!ph_auth_pair_resume(auth,src,key,secret)) error="unauthorized";
            if(!error)for(unsigned i=0;i<sizeof(u->peers)/sizeof(*u->peers);++i){
                ph_udp_peer *p=&u->peers[i];if(!p->active||strcmp(p->peer,src)||p->boot==boot)continue;
                for(unsigned k=0;k<4;++k)if(p->retired[k]==boot){memset(key,0,sizeof(key));memset(secret,0,sizeof(secret));return 0;}
                /* An authenticated resume after the controller reboots rotates
                 * its token; old boot/sequence packets cannot regain control. */
                ph_auth_context ctx;
                if(!ph_auth_udp_check(auth,src,secret,&ctx)||ctx.source!=PH_AUTH_UDP_PAIR||ctx.slot>=PH_AUTH_PAIR_SLOTS){memset(key,0,sizeof(key));return 0;}
                memset(&auth->local[ctx.slot],0,sizeof(auth->local[ctx.slot]));
                if(!ph_auth_pair_resume(auth,src,key,secret)){memset(key,0,sizeof(key));return 0;}
                memmove(p->retired+1,p->retired,3*sizeof(*p->retired));p->retired[0]=p->boot;
                p->boot=boot;p->last=0;strcpy(p->token,secret);break;
            }
            memset(key,0,sizeof(key));
        } else if (!strcmp(op,"command") || !strcmp(op,"probe") || !strcmp(op,"read")) {
            ph_auth_context ctx; reply=!strcmp(op,"command")?"result":!strcmp(op,"probe")?"pong":"sample";
            if (!text(&v,"token",token,sizeof(token)) || !ph_auth_udp_check(auth,src,token,&ctx) || !ph_auth_context_valid(&ctx,!strcmp(op,"command")?PH_AUTH_SCOPE_CONTROL:PH_AUTH_SCOPE_SENSOR)) return 0;
            ph_udp_peer *p = NULL;
            for (unsigned i=0;i<sizeof(u->peers)/sizeof(*u->peers);++i) {
                if (u->peers[i].active && !strcmp(src,u->peers[i].peer)) { p=&u->peers[i]; break; }
                if (!u->peers[i].active && !p) p=&u->peers[i];
            }
            if (!p) return 0;
            if(p->active)for(unsigned k=0;k<4;++k)if(p->retired[k]==boot)return 0;
            if (p->active && !strcmp(p->token,token) && (p->boot != boot || id <= p->last)) return 0;
            p->active=true; strcpy(p->peer,src); strcpy(p->token,token); p->boot=boot; p->last=id;
            if(!strcmp(op,"command"))data_n=ph_command(d,body.begin,(size_t)(body.end-body.begin),f,f,true,false,data,sizeof(data));
            else {if(body.count)return 0;data_n=ph_query(d,"status.get",f,data,sizeof(data));}
        } else return 0;
    }
    if (data_n && !phv_parse(f,data,data_n,&value)) return 0;
    phw_init_fixed(&w,f,&b,out,cap); phw_begin(&w,NULL,false);
    phw_u32(&w,"v",2); phw_text(&w,"src",d->mac); phw_text(&w,"dst",src);
    phw_u32(&w,"boot",d->boot); phw_u32(&w,"id",id); phw_text(&w,"op",reply);
    phw_text(&w,"token",(!strcmp(op,"auth.pair")||!strcmp(op,"auth.resume"))?"":token);
    if (data_n) phw_value(&w,"body",&value);
    else {
        phw_begin(&w,"body",false);
        if (error) phw_text(&w,"error",error);
        else phw_text(&w,!strcmp(op,"auth.pair")?"key":"token",secret);
        phw_end(&w);
    }
    phw_end(&w); memset(secret,0,sizeof(secret)); return phw_finish_fixed(&w);
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
/* HTTP owner adapter. Auth state and durable pairs belong to product main. */
#include "../local_auth.h"
#include "../typed_wire.h"

static void http_auth_reply(ph_http_product *p,ph_http_exchange *x,unsigned status,
                            const char *code,const char *token)
{
    ph_writer w;ph_buf b;
    phw_init_fixed(&w,PH_WIRE_MSGPACK,&b,p->scalar,sizeof(p->scalar));
    phw_begin(&w,NULL,false);phw_text(&w,"code",code);
    if(token){phw_begin(&w,"data",false);phw_text(&w,"token",token);phw_end(&w);}
    phw_end(&w);
    x->response.status=status;x->response.type="application/msgpack";
    x->response.body=p->scalar;x->response.length=phw_finish_fixed(&w);
}
static bool http_auth_authorized(const char *authorization)
{
    ph_auth_context context;ph_auth_state *s=device_local_auth();
    return s&&ph_auth_http_check(s,authorization,&context);
}
static bool http_auth_route(const char *path)
{
    return !strcmp(path,"/api/v1/auth/login")||!strcmp(path,"/api/v1/auth/logout");
}
static void http_auth_complete(ph_http_product *p,ph_http_exchange *x)
{
    ph_auth_state *s=device_local_auth();ph_value request,value;
    if(!s){http_auth_reply(p,x,503,"busy",NULL);return;}
    if(!phv_parse(PH_WIRE_MSGPACK,p->body,p->used,&request)||request.kind!=PHV_MAP){
        http_auth_reply(p,x,400,"invalid_request",NULL);return;
    }
    if(!strcmp(x->request->path,"/api/v1/auth/logout")){
        if(!phv_keys(&request,"")){http_auth_reply(p,x,400,"invalid_request",NULL);return;}
        bool ok=ph_auth_http_logout(s,x->request->authorization);
        http_auth_reply(p,x,ok?200:401,ok?"ok":"unauthorized",NULL);return;
    }
    char password[33]={0},token[33]={0};
    if(!phv_keys(&request,"|password|")||!phv_get(&request,"password",&value)||
       !phv_text(&value,password,sizeof(password))){
        http_auth_reply(p,x,400,"invalid_request",NULL);return;
    }
    ph_auth_login_result result=ph_auth_http_login_result(s,password,token);
    http_auth_reply(p,x,result==PH_AUTH_LOGIN_OK?200:result==PH_AUTH_LOGIN_DENIED?401:503,
                    result==PH_AUTH_LOGIN_OK?"ok":result==PH_AUTH_LOGIN_DENIED?"unauthorized":"busy",
                    result==PH_AUTH_LOGIN_OK?token:NULL);
    memset(password,0,sizeof(password));memset(token,0,sizeof(token));
}
/* Public discovery intentionally excludes config, Wi-Fi and broker data. */
static void http_auth_info(ph_http_product *p,ph_http_exchange *x)
{
    ph_auth_state *s=device_local_auth();ph_writer w;ph_buf b;
    if(!s){http_auth_reply(p,x,503,"busy",NULL);return;}
    bool password_set=ph_auth_password_is_set(s);
    phw_init_fixed(&w,PH_WIRE_MSGPACK,&b,p->scalar,sizeof(p->scalar));
    phw_begin(&w,NULL,false);phw_text(&w,"code","ok");phw_begin(&w,"data",false);
    phw_u32(&w,"api_version",1);phw_text(&w,"model",HTTP_AUTH_MODEL);
    phw_text(&w,"protocol","tmcp");phw_u32(&w,"protocol_version",2);
    phw_text(&w,"device_id",HTTP_AUTH_ID);phw_text(&w,"firmware_version",HTTP_AUTH_VERSION);
    phw_text(&w,"version",HTTP_AUTH_VERSION);phw_text(&w,"variant",PHICOMM_FIRMWARE_VARIANT);
    phw_text(&w,"firmware_variant",PHICOMM_FIRMWARE_VARIANT);phw_u32(&w,"wire_version",1);
    phw_begin(&w,"wire_formats",true);phw_text(&w,NULL,"msgpack");phw_end(&w);
    phw_text(&w,"default_wire_format","msgpack");
    phw_text(&w,"auth","bearer");phw_bool(&w,"password_set",password_set);
    phw_end(&w);phw_end(&w);
    x->response.status=200;x->response.type="application/msgpack";
    x->response.body=p->scalar;x->response.length=phw_finish_fixed(&w);
}

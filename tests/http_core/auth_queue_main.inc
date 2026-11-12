static void auth_code(unsigned expected,const char *code){
    ph_value root=parsed(expected),v;assert(phv_get(&root,"code",&v)&&phv_equal_text(&v,code));
}
static void auth_header(unsigned mode,char out[128]){
    assert(credentials(mode?"pw":"",true));out[0]=0;
    if(mode==1)strcpy(out,"Basic YWRtaW46cHc=");
    if(mode==2){char token[33];assert(ph_auth_http_login(&auth_state,"pw",token));snprintf(out,128,"Bearer %s",token);}
}
int main(void){
    ph_auth_ops ops={0};ops.now=auth_now;ops.random=auth_random;ops.password=auth_password;
    assert(ph_auth_init(&auth_state,&ops,NULL));assert(MODEL_START()==0);
    execute_wire("GET /api/v1/status HTTP/1.1\r\n\r\n");assert(strstr(wire_output,"200 OK"));
    const unsigned char empty_login[]={0x81,0xa8,'p','a','s','s','w','o','r','d',0xa0};
    for(unsigned i=0;i<12;i++){
        assert(start_auth("POST","/api/v1/auth/login",sizeof(empty_login),NULL)==PH_HTTP_READ_BODY);
        assert(app.body(&x,empty_login,sizeof(empty_login)));app.complete(&x);auth_code(200,"ok");
        ph_value root=parsed(200),data,t,v;assert(phv_get(&root,"data",&data));
        assert(phv_get(&data,"token",&t)&&phv_equal_text(&t,""));
        assert(phv_get(&data,"password_required",&v)&&v.kind==PHV_FALSE);finish(true);
    }
    assert(!auth_random_calls);
    for(unsigned i=0;i<PH_AUTH_HTTP_SLOTS;i++)assert(!auth_state.http[i].active);
    for(unsigned mode=0;mode<3;mode++){
        char header[128];auth_header(mode,header);
        /* Ordinary requests establish a fresh context on every product begin. */
        assert(start_auth("GET","/api/v1/status",0,header)==PH_HTTP_REPLY);parsed(200);finish(true);
        assert(start_auth("POST","/api/v1/command",sizeof(post),header)==PH_HTTP_READ_BODY);
        assert(app.body(&x,post,sizeof(post)));unsigned before=commands;app.complete(&x);
        parsed(200);assert(commands==before+1);finish(true);

        auth_header(mode,header);deny=true;before=queries;
        assert(start_auth("GET","/api/v1/status",0,header)==PH_HTTP_WAIT&&waiting);
        assert(credentials(mode?"pw":"",true));deny=false;
        assert(app.retry(&x)==PH_HTTP_REPLY);auth_code(401,"unauthorized");
        assert(queries==before);finish(false); /* same-password restore */

        auth_header(mode,header);before=commands;
        assert(start_auth("POST","/api/v1/command",sizeof(post),header)==PH_HTTP_READ_BODY);
        assert(app.body(&x,post,3));assert(credentials(mode?"pw":"",true));
        assert(app.body(&x,post+3,sizeof(post)-3));app.complete(&x);auth_code(401,"unauthorized");
        assert(commands==before);finish(false);

        auth_header(mode,header);deny=true;
        assert(start_auth("GET","/api/v1/status",0,header)==PH_HTTP_WAIT);
        auth_fail_commit=true;assert(!credentials(mode?"pw":"",true));auth_fail_commit=false;
        assert(credentials(mode?"pw":"",false));deny=false;
        assert(app.retry(&x)==PH_HTTP_REPLY);parsed(200);finish(true); /* failed/no-op commit */

        auth_header(mode,header);deny=true;
        assert(start_auth("GET","/api/v1/status",0,header)==PH_HTTP_WAIT);
        assert(credentials(mode?"":"pw",false));assert(credentials(mode?"pw":"",false));deny=false;
        assert(app.retry(&x)==PH_HTTP_REPLY);auth_code(401,"unauthorized");finish(false);
    }
    char header[128];auth_header(2,header);deny=true;
    assert(start_auth("GET","/api/v1/status",0,header)==PH_HTTP_WAIT);
    assert(ph_auth_http_logout(&auth_state,header));deny=false;
    assert(app.retry(&x)==PH_HTTP_REPLY);auth_code(401,"unauthorized");finish(false);
    auth_header(2,header);deny=true;
    assert(start_auth("GET","/api/v1/status",0,header)==PH_HTTP_WAIT);
    tick+=PH_AUTH_IDLE_MS;deny=false;
    assert(app.retry(&x)==PH_HTTP_REPLY);auth_code(401,"unauthorized");finish(false);
    assert(credentials("pw",true));
    execute_wire("GET /api/v1/status HTTP/1.1\r\n\r\n");assert(strstr(wire_output,"401 Unauthorized"));
    execute_wire("GET /api/v1/status HTTP/1.1\r\nAuthorization: Basic YWRtaW46cHc=\r\n\r\n");assert(strstr(wire_output,"200 OK"));
    puts("real product HTTP auth: open/Basic/Bearer; queued wait/body restore, rollback, expiry, logout PASS");
    return 0;
}

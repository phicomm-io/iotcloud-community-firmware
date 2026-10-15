/* Built-in discovery is absent unless a source was compiled in. Manual signed
 * URL/file OTA remains a separate recovery path. Included by service.c. */
#if defined(PHICOMM_OTA_CN_BASE_URL) || defined(PHICOMM_OTA_GLOBAL_BASE_URL)
#ifndef PHICOMM_OTA_CN_BASE_URL
#define PHICOMM_OTA_CN_BASE_URL ""
#endif
#ifndef PHICOMM_OTA_GLOBAL_BASE_URL
#define PHICOMM_OTA_GLOBAL_BASE_URL ""
#endif
#if defined(AUTOMATION_TC1)
#define UPDATE_MODEL "tc1"
#elif defined(FEATURE_DEVICE_A1)
#define UPDATE_MODEL "a1"
#else
#define UPDATE_MODEL "m1"
#endif
#include "../generated/build_version.h"
static const char *update_bases[]={PHICOMM_OTA_CN_BASE_URL,PHICOMM_OTA_GLOBAL_BASE_URL};
static const char *update_sources[]={"cn","global"};
static bool update_busy, update_available;
static uint32_t update_deadline;
static unsigned update_source;
static char update_state[24]="idle", update_version[32], update_path[200], update_notes[200];
static int update_read(int fd,void *out,size_t n) {
    int32_t remaining=(int32_t)(update_deadline-mico_rtos_get_time());if(remaining<=0)return -1;
    unsigned wait=remaining>10000?10000:(unsigned)remaining;
    fd_set f;FD_ZERO(&f);FD_SET(fd,&f);struct timeval tv={wait/1000,(wait%1000)*1000};
    if(select(fd+1,&f,NULL,NULL,&tv)<=0)return -1;return recv(fd,out,n,0);
}
static int update_line(int fd,char *out,size_t cap) {
    size_t n=0;uint32_t deadline=mico_rtos_get_time()+10000;
    while(n+1<cap){char c;if((int32_t)(mico_rtos_get_time()-deadline)>=0||update_read(fd,&c,1)!=1||!c)return -1;
        if(c=='\n'){if(!n||out[n-1]!='\r')return -1;out[--n]=0;return n;}out[n++]=c;}return -1;
}
static bool update_semver(const char *s,unsigned v[3]) {
    for(int i=0;i<3;i++){if(!isdigit((unsigned char)*s))return false;v[i]=0;while(isdigit((unsigned char)*s)){if(v[i]>100000)return false;v[i]=v[i]*10+(*s++-'0');}if(i<2&&*s++!='.')return false;}
    return !*s||*s=='-';
}
static const char *update_text(json_object *o,const char *key) {
    json_object *v=json_object_object_get(o,key);return v&&json_object_is_type(v,json_type_string)?json_object_get_string(v):"";
}
static bool update_relative(const char *s) {
    if(strncmp(s,"/releases/v",11)||strlen(s)>=200||strstr(s,".."))return false;
    for(;*s;s++)if(!isalnum((unsigned char)*s)&&!strchr("/._-",*s))return false;return true;
}
static void update_worker(mico_thread_arg_t arg) {
    (void)arg;update_deadline=mico_rtos_get_time()+30000;int fd=-1;json_object *meta=NULL;char *body=NULL;
    char host[161],line[512];unsigned selected=update_source,port=80;
    const char *base=update_bases[selected];snprintf(host,sizeof(host),"%s",base+7);
    char *colon=strchr(host,':');if(colon){*colon++=0;port=(unsigned)atoi(colon);}
    struct hostent *resolved=gethostbyname(host);if(!resolved||!resolved->h_addr_list[0])goto done;
    struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_port=htons(port);memcpy(&address.sin_addr,resolved->h_addr_list[0],4);
    fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(fd<0)goto done;
    int timeout=10000;setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    if(connect(fd,(struct sockaddr *)&address,sizeof(address))<0)goto done;
    snprintf(line,sizeof(line),"GET /ota/" UPDATE_MODEL "/latest.json HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\nAccept-Encoding: identity\r\n\r\n",host,port);
    size_t sent=0,n=strlen(line);while(sent<n){int rc=send(fd,line+sent,n-sent,0);if(rc<=0)goto done;sent+=rc;}
    if(update_line(fd,line,sizeof(line))<0||(strncmp(line,"HTTP/1.1 200 ",13)&&strncmp(line,"HTTP/1.0 200 ",13)))goto done;
    unsigned length=0,headers=0;bool have=false;
    for(;;){int rc=update_line(fd,line,sizeof(line));if(rc<0||(headers+=rc)>4096)goto done;if(!rc)break;
        if(!strncasecmp(line,"Transfer-Encoding:",18)||!strncasecmp(line,"Content-Encoding:",17))goto done;
        if(!strncasecmp(line,"Content-Length:",15)){char *end;unsigned long v=strtoul(line+15,&end,10);while(*end==' ')end++;if(have||*end||v<2||v>4096)goto done;length=v;have=true;}}
    if(!have)goto done;body=malloc(length+1);if(!body)goto done;
    for(unsigned got=0;got<length;){int rc=update_read(fd,body+got,length-got);if(rc<=0)goto done;got+=rc;}body[length]=0;
    meta=service_parse(body,length);if(!meta)goto done;
    const char *version=update_text(meta,"version"),*path=update_text(meta,"path"),*notes=update_text(meta,"release_log");unsigned remote[3],local[3];
    if(strcmp(update_text(meta,"model"),UPDATE_MODEL)||!update_semver(version,remote)||!update_semver(FIRMWARE_VERSION,local)||strlen(version)>=sizeof(update_version)||!update_relative(path)||!update_relative(notes))goto done;
    mico_rtos_lock_mutex(&service_lock);
    strcpy(update_version,version);strcpy(update_path,path);strcpy(update_notes,notes);update_available=false;
    for(int i=0;i<3;i++){if(remote[i]!=local[i]){update_available=remote[i]>local[i];break;}}
    strcpy(update_state,update_available?"available":"up_to_date");
    mico_rtos_unlock_mutex(&service_lock);
done:
    if(fd>=0)close(fd);if(meta)json_object_put(meta);free(body);
    mico_rtos_lock_mutex(&service_lock);if(!strcmp(update_state,"checking"))strcpy(update_state,"failed");update_busy=false;mico_rtos_unlock_mutex(&service_lock);
    mico_rtos_delete_thread(NULL);
}
static void update_capabilities(json_object *o) {
    json_object *a=json_object_object_get(o,"actions");if(a){const char *names[]={"ota.sources.get","ota.check","ota.check.get","ota.install"};for(unsigned i=0;i<4;i++)json_object_array_add(a,json_object_new_string(names[i]));}
}
static json_object *update_command(const char *action,json_object *p,const char **code,bool *handled) {
    if(strcmp(action,"ota.sources.get")&&strcmp(action,"ota.check")&&strcmp(action,"ota.check.get")&&strcmp(action,"ota.install"))return NULL;
    *handled=true;*code="ok";json_object *o=json_object_new_object();
    if(!strcmp(action,"ota.sources.get")){json_object *a=json_object_new_array();for(unsigned i=0;i<2;i++)if(*update_bases[i])json_object_array_add(a,json_object_new_string(update_sources[i]));json_object_object_add(o,"sources",a);return o;}
    mico_rtos_lock_mutex(&service_lock);
    if(!strcmp(action,"ota.check")){
        const char *source=update_text(p,"source");int selected=!strcmp(source,"cn")?0:!strcmp(source,"global")?1:-1;
        if(selected<0||!*update_bases[selected])*code="invalid_params";
        else if(update_busy)*code="busy";
        else {update_source=selected;update_busy=true;update_available=false;update_version[0]=update_path[0]=update_notes[0]=0;strcpy(update_state,"checking");
            if(mico_rtos_create_thread(NULL,MICO_APPLICATION_PRIORITY,"ota_check",update_worker,6144,0)!=kNoErr){update_busy=false;strcpy(update_state,"failed");*code="busy";}else *code="accepted";}
    } else if(!strcmp(action,"ota.install")){
        if(update_busy||!update_available)*code="invalid_params";
        else {char url[400];snprintf(url,sizeof(url),"%s%s",update_bases[update_source],update_path);json_object *params=json_object_new_object();string(params,"url",url);
            mico_rtos_unlock_mutex(&service_lock);json_object_put(o);o=service_product("ota.start",params,code);json_object_put(params);return o;}
    }
    string(o,"state",update_state);string(o,"version",update_version);string(o,"source",update_sources[update_source]);
    if(*update_notes){char url[400];snprintf(url,sizeof(url),"%s%s",update_bases[update_source],update_notes);string(o,"release_log",url);}
    json_object_object_add(o,"available",json_object_new_boolean(update_available));mico_rtos_unlock_mutex(&service_lock);return o;
}
#else
static void update_capabilities(json_object *o){(void)o;}
static json_object *update_command(const char *a,json_object *p,const char **code,bool *handled){(void)a;(void)p;(void)code;(void)handled;return NULL;}
#endif

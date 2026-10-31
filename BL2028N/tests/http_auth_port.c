/* Actual HTTP -> device -> platform credential journal, with LFS faults. */
#include <assert.h>
#include <errno.h>
#include "platform_host.h"
static unsigned fail_stage,write_attempts,raw_mutations,transactions;
static bool in_command;
static lfs_file_t *auth_output;
static int auth_open(lfs_t *,lfs_file_t *,const char *,int);
static lfs_ssize_t auth_write(lfs_t *,lfs_file_t *,const void *,size_t);
static int auth_sync(lfs_t *,lfs_file_t *);
static int auth_close(lfs_t *,lfs_file_t *);
static int auth_rename(lfs_t *,const char *,const char *);
#define lfs_file_open auth_open
#define lfs_file_write auth_write
#define lfs_file_sync auth_sync
#define lfs_file_close auth_close
#define lfs_rename auth_rename
#include "../src/ph_port.c"
#undef lfs_file_open
#undef lfs_file_write
#undef lfs_file_sync
#undef lfs_file_close
#undef lfs_rename
/* Simulated cold boots reset the real mDNS module's BSS as well as the port. */
#include "../src/ph_mdns.c"
#ifndef PH_AUTH_HTTP_STATELESS
#error Synchronize the reviewed common auth snapshot before this regression
#endif
extern const ph_http_app bl_http_app;
extern char web_password[33];
extern char wifi_pass2[64];
static struct printbuf *watched_response;
static unsigned retained_frees;
static ph_auth_context sensor_context;
static ph_auth_session sensor_grants[PH_AUTH_MQTT_SLOTS];
void __real_free(void *);
void __wrap_free(void *p){
 if(p&&p==watched_response){
  assert(udp_state.server.response==p&&"retained owner cleared before release");
  retained_frees++;watched_response=NULL;
 }
 __real_free(p);
}
void bl_mqtt_start(void){}
void bl_mqtt_diagnostics(ph_writer *w){(void)w;}
int __wrap_rtos_create_thread(beken_thread_t *t,int priority,const char *name,
 void (*fn)(beken_thread_arg_t),int stack,beken_thread_arg_t arg){
 (void)t;(void)priority;(void)name;(void)fn;(void)stack;(void)arg;return 0;
}
void __real_rtos_unlock_mutex(beken_mutex_t *);
void __wrap_rtos_unlock_mutex(beken_mutex_t *m){
 assert(!in_command&&"platform command released its product lock");
 __real_rtos_unlock_mutex(m);
}
void __real_PH_BL_SetWebPasswordMemory(const char *);
void __wrap_PH_BL_SetWebPasswordMemory(const char *p){
 if(bl_ready)assert(pthread_mutex_trylock(&mutex)==EBUSY);
 if(in_command)raw_mutations++;
 __real_PH_BL_SetWebPasswordMemory(p);
}
size_t __real_ph_command(ph_device *,const void *,size_t,ph_wire_format,
 ph_wire_format,bool,bool,void *,size_t);
size_t __wrap_ph_command(ph_device *d,const void *p,size_t n,ph_wire_format input,
 ph_wire_format output,bool authorized,bool admin,void *out,size_t capacity){
 assert(!in_command);in_command=true;
 size_t result=__real_ph_command(d,p,n,input,output,authorized,admin,out,capacity);
 in_command=false;return result;
}
static int auth_open(lfs_t *fs,lfs_file_t *f,const char *name,int flags){
 bool output=!strcmp(name,".phicomm.tmp");
 if(output){write_attempts++;if(fail_stage==1)return -1;}
 int rc=lfs_file_open(fs,f,name,flags);if(!rc&&output)auth_output=f;return rc;
}
static lfs_ssize_t auth_write(lfs_t *fs,lfs_file_t *f,const void *p,size_t n){
 return lfs_file_write(fs,f,p,n-(f==auth_output&&fail_stage==2));
}
static int auth_sync(lfs_t *fs,lfs_file_t *f){return f==auth_output&&fail_stage==3?-1:lfs_file_sync(fs,f);}
static int auth_close(lfs_t *fs,lfs_file_t *f){
 bool output=f==auth_output;int rc=lfs_file_close(fs,f);if(output)auth_output=NULL;
 return output&&fail_stage==4?-1:rc;
}
static int auth_rename(lfs_t *fs,const char *a,const char *b){return fail_stage==5?-1:lfs_rename(fs,a,b);}
static size_t read_record(unsigned char *out,size_t capacity){
 lfs_file_t f;assert(!lfs_file_open(&lfs,&f,".phicomm",LFS_O_RDONLY));
 lfs_soff_t n=lfs_file_size(&lfs,&f);assert(n>=0&&(size_t)n<=capacity);
 assert(lfs_file_read(&lfs,&f,out,(size_t)n)==n);assert(!lfs_file_close(&lfs,&f));return (size_t)n;
}
static void boot(void){
 if(bl_ready){ph_lan_v4_revoke(&udp_state);ph_status_cache_release(&bl_device);assert(!pthread_mutex_destroy(&mutex));}
 bl_ready=false;credential_owned=false;memset(committed_password,0,sizeof(committed_password));
 identity_ready=false;memset(&identity,0,sizeof(identity));active=failures=0;
 memset(&udp_state,0,sizeof(udp_state));PH_BL_Init(0);assert(bl_ready);
}
static void preserved_settings(const ph_settings *before){
 ph_settings current=bl_device.settings;current.auth=before->auth;current.energy_kwh=before->energy_kwh;
 assert(!memcmp(&current,before,sizeof(current)));
 assert(!strcmp(CFG_GetWiFiSSID(),"lab")&&!strcmp(CFG_GetWiFiPass(),"test-password"));
 assert(!strcmp(CFG_GetWiFiSSID2(),"backup-lab")&&!strcmp(wifi_pass2,"backup-password"));
 assert(!strcmp(CFG_GetMQTTHost(),"127.0.0.1")&&CFG_GetMQTTPort()==1883);
 assert(!strcmp(CFG_GetMQTTUserName(),"test-user")&&!strcmp(CFG_GetMQTTPass(),"test-pass"));
}
static void seed(const char *directory,const char *initial){
 snprintf(store_dir,sizeof(store_dir),"%s",directory);assert(!mkdir(store_dir,0700));
 snprintf(web_password,sizeof(web_password),"%s",initial);fail_stage=0;boot();
 CFG_SetWiFiSSID2("backup-lab");CFG_SetWiFiPass2("backup-password");CFG_SetMQTTPort(1883);
 bl_device.settings.revision=19;bl_device.settings.child_lock=1;bl_device.settings.indicator=0;
 bl_device.settings.power_on=1;bl_device.settings.calibrated=1;bl_device.settings.report_seconds=37;
 strcpy(bl_device.settings.name,"auth-port-device");strcpy(bl_device.settings.socket_name,"auth-port-outlet");
 bl_device.settings.coefficients[0]=1.234;bl_device.settings.coefficients[1]=2.345;bl_device.settings.coefficients[2]=3.456;
 bl_device.meter.energy_kwh=876.125;bl_device.settings.energy_kwh=876.125;
 char key[33];assert(ph_auth_pair(&bl_auth,initial,"001122334455",PH_AUTH_SCOPE_CONTROL,key));
 assert(ph_auth_remote_store(&bl_auth,"112233445566","0102030405060708090a0b0c0d0e0f10",PH_AUTH_SCOPE_CONTROL,ph_auth_credential_epoch(&bl_auth)));
 assert(bl_auth.store.count==1&&bl_auth.store.remote_count==1);
 lock();assert(bl_sensor_grant("223344556677","102030405060708090a0b0c0d0e0f001",91));unlock();
 assert(ph_auth_udp_check(&bl_auth,"223344556677","102030405060708090a0b0c0d0e0f001",&sensor_context));
 assert(sensor_context.source==PH_AUTH_UDP_MQTT&&sensor_context.scopes==PH_AUTH_SCOPE_SENSOR);
 memcpy(sensor_grants,bl_auth.mqtt,sizeof(sensor_grants));
}
static void sensor_result(bool changed){
 if(changed){ph_auth_session empty[PH_AUTH_MQTT_SLOTS]={0};assert(!memcmp(bl_auth.mqtt,empty,sizeof(empty)));}
 else assert(!memcmp(bl_auth.mqtt,sensor_grants,sizeof(sensor_grants)));
 assert(ph_auth_context_valid(&sensor_context,PH_AUTH_SCOPE_SENSOR)==!changed);
 assert(!ph_auth_context_valid(&sensor_context,PH_AUTH_SCOPE_CONTROL));
}
static void retain_v4(const char *token){
 assert(!watched_response&&!udp_state.server.response);
 unsigned char body[256],input[LAN_V4_FRAME_MAX],frame[LAN_V4_FRAME_MAX];
 ph_writer writer;struct printbuf buffer;
 assert(phw_init_fixed(&writer,PH_WIRE_MSGPACK,&buffer,body,sizeof(body)));
 phw_begin(&writer,NULL,false);phw_text(&writer,"request_id","auth-retained");
 phw_text(&writer,"action","status.get");phw_begin(&writer,"params",false);phw_end(&writer);phw_end(&writer);
 size_t body_length=phw_finish_fixed(&writer);assert(body_length);
 lan_v4_packet request={0};request.op=LAN_V4_COMMAND;request.boot=77;request.id=1;
 strcpy(request.src,"001122334455");strcpy(request.dst,bl_device.mac);
 for(unsigned i=0;i<16;i++){unsigned byte;assert(sscanf(token+2*i,"%2x",&byte)==1);request.token[i]=(uint8_t)byte;}
 request.body=body;request.body_length=body_length;
 assert(phw_init_fixed(&writer,PH_WIRE_MSGPACK,&buffer,input,sizeof(input)));
 assert(lan_v4_encode(&writer,&request));size_t input_length=phw_finish_fixed(&writer);assert(input_length);
 lock();size_t n=ph_lan_v4_feed(&udp_state,input,input_length,"127.0.0.2",12345,frame,sizeof(frame));unlock();
 lan_v4_packet result;assert(n&&lan_v4_decode(frame,n,&result)&&!strcmp(result.code,"ok"));
 assert(udp_state.server.response&&udp_state.server.response->bpos>0);
 watched_response=udp_state.server.response;
}
static void retain_paired_v4(void){
 char key[33],token[33];assert(ph_auth_pair_key(&bl_auth,"001122334455",key));
 assert(ph_auth_pair_resume(&bl_auth,"001122334455",key,token));retain_v4(token);
}
static void retained_result(const lan_v4_server *before,unsigned frees,bool changed){
 if(changed){lan_v4_server empty={0};assert(!memcmp(&udp_state.server,&empty,sizeof(empty)));
  assert(!watched_response&&retained_frees==frees+1);
 }else{assert(watched_response==before->response&&retained_frees==frees);
  assert(!memcmp(&udp_state.server,before,sizeof(*before)));
 }
}
static void make_header(unsigned mode,char header[64]){
 if(!mode)header[0]=0;
 else if(mode==1)strcpy(header,"Basic YWRtaW46c2VjcmV0MTIz");
 else {char token[33];assert(ph_auth_http_login(&bl_auth,"secret123",token));snprintf(header,64,"Bearer %s",token);}
}
static void set_password_http_request(const char *header,const char *next,ph_wire_format format,const char *code,const char *id){
 unsigned char bytes[512];ph_writer writer;struct printbuf buffer;
 assert(phw_init_fixed(&writer,format,&buffer,bytes,sizeof(bytes)));phw_begin(&writer,NULL,false);
 phw_text(&writer,"request_id",id);phw_text(&writer,"action","config.set");
 phw_begin(&writer,"params",false);phw_text(&writer,"section","auth");phw_begin(&writer,"values",false);
 phw_text(&writer,"password",next);phw_end(&writer);phw_end(&writer);phw_end(&writer);
 size_t n=phw_finish_fixed(&writer);assert(n);
 ph_http_request request={0};ph_http_exchange exchange={.request=&request};
 strcpy(request.method,"POST");strcpy(request.path,"/api/v1/command");strcpy(request.authorization,header);
 request.have_accept=request.have_type=1;request.accept_format=request.content_format=format;
 request.have_length=true;request.length=(uint32_t)n;
 assert(bl_http_app.begin(&exchange)==PH_HTTP_READ_BODY);assert(bl_http_app.body(&exchange,bytes,n));
 bl_http_app.complete(&exchange);assert(exchange.response.status==200);
 ph_value root,value;assert(phv_parse(format,exchange.response.body,exchange.response.length,&root));
 assert(phv_get(&root,"code",&value));
 if(!phv_equal_text(&value,code)){char actual[40];assert(phv_text(&value,actual,sizeof(actual)));fprintf(stderr,"transaction %u returned %s, expected %s\n",transactions,actual,code);}
 assert(phv_equal_text(&value,code));bl_http_app.release(&exchange,true);
}
static void set_password_http(const char *header,const char *next,ph_wire_format format,const char *code){
 set_password_http_request(header,next,format,code,"port-credential");
}
static void transaction(const char *root,unsigned mode,unsigned stage,ph_wire_format format){
 char directory[1024],header[64];snprintf(directory,sizeof(directory),"%s/case-%u-%u-%u",root,mode,stage,(unsigned)format);
 const char *initial=mode?"secret123":"",*next=stage==6?initial:"changed-password";
 seed(directory,initial);make_header(mode,header);ph_auth_context original;
 assert(ph_auth_http_check(&bl_auth,header,&original));
 char pair_key[33],local_token[33];assert(ph_auth_pair_key(&bl_auth,"001122334455",pair_key));
 assert(ph_auth_pair_resume(&bl_auth,"001122334455",pair_key,local_token));ph_auth_context local;
 assert(ph_auth_udp_check(&bl_auth,"001122334455",local_token,&local));
 retain_v4(local_token);lan_v4_server retained=udp_state.server;unsigned frees=retained_frees;
 uint32_t epoch=ph_auth_credential_epoch(&bl_auth);ph_settings before=bl_device.settings;
 unsigned char old_file[sizeof(disk_record)],new_file[sizeof(disk_record)];size_t old_size=read_record(old_file,sizeof(old_file));
 unsigned writes=write_attempts,raw=raw_mutations;fail_stage=stage<6?stage:0;
 set_password_http(header,next,format,stage&&stage<6?"storage_error":"accepted");fail_stage=0;
 bool changed=stage==0;assert(!strcmp(CFG_GetWebPassword(),changed?next:initial));preserved_settings(&before);
 retained_result(&retained,frees,changed);sensor_result(changed);
 assert(ph_auth_credential_epoch(&bl_auth)==epoch+(unsigned)changed);
 assert(ph_auth_context_valid(&original,PH_AUTH_SCOPE_CONTROL)==!changed);
 assert(ph_auth_context_valid(&local,PH_AUTH_SCOPE_CONTROL)==!changed);
 assert(bl_auth.store.count==(changed?0:1)&&bl_auth.store.remote_count==(changed?0:1));
 assert(raw_mutations==raw+(unsigned)changed);
 if(stage==6)assert(write_attempts==writes); /* Identical password is a true no-op. */
 if(!changed){size_t n=read_record(new_file,sizeof(new_file));assert(n==old_size&&!memcmp(old_file,new_file,n));}
 /* Model an SDK configuration save that never reached durable storage. */
 strcpy(web_password,initial);boot();assert(!strcmp(CFG_GetWebPassword(),changed?next:initial));preserved_settings(&before);
 assert(bl_auth.store.count==(changed?0:1)&&bl_auth.store.remote_count==(changed?0:1));
 char token[33];assert(ph_auth_http_login(&bl_auth,changed?next:initial,token));
 transactions++;
}
static void empty_password_recovery(const char *root,ph_wire_format format){
 char directory[1024];snprintf(directory,sizeof(directory),"%s/empty-%u",root,(unsigned)format);
 seed(directory,"secret123");retain_paired_v4();lan_v4_server retained=udp_state.server;unsigned frees=retained_frees;
 set_password_http("Basic YWRtaW46c2VjcmV0MTIz","",format,"accepted");retained_result(&retained,frees,true);sensor_result(true);
 assert(credential_owned&&!*CFG_GetWebPassword());strcpy(web_password,"stale-sdk-password");boot();
 assert(!*CFG_GetWebPassword()&&credential_owned);char token[33];assert(ph_auth_http_login(&bl_auth,"",token)&&!*token);
 assert(!bl_auth.store.count&&!bl_auth.store.remote_count);
}
static void sdk_password_transaction(const char *root,unsigned stage){
 char directory[1024];snprintf(directory,sizeof(directory),"%s/sdk-%u",root,stage);seed(directory,"secret123");
 char token[33],header[64];assert(ph_auth_http_login(&bl_auth,"secret123",token));snprintf(header,sizeof(header),"Bearer %s",token);
 ph_auth_context original;assert(ph_auth_http_check(&bl_auth,header,&original));
 retain_paired_v4();lan_v4_server retained=udp_state.server;unsigned frees=retained_frees;
 uint32_t epoch=ph_auth_credential_epoch(&bl_auth);ph_settings before=bl_device.settings;
 unsigned char old_file[sizeof(disk_record)],new_file[sizeof(disk_record)];size_t old_size=read_record(old_file,sizeof(old_file));
 unsigned writes=write_attempts;bool changed=stage==0;const char *next=stage==6?"secret123":"sdk-password";
 fail_stage=stage<6?stage:0;CFG_SetWebPassword(next);fail_stage=0;
 retained_result(&retained,frees,changed);sensor_result(changed);
 assert(!strcmp(CFG_GetWebPassword(),changed?next:"secret123"));
 assert(ph_auth_credential_epoch(&bl_auth)==epoch+(unsigned)changed);
 assert(ph_auth_context_valid(&original,PH_AUTH_SCOPE_CONTROL)==!changed);preserved_settings(&before);
 if(stage==6)assert(write_attempts==writes);
 if(!changed){size_t n=read_record(new_file,sizeof(new_file));assert(n==old_size&&!memcmp(old_file,new_file,n));}
 strcpy(web_password,"secret123");boot();assert(!strcmp(CFG_GetWebPassword(),changed?next:"secret123"));preserved_settings(&before);
 assert(bl_auth.store.count==(changed?0:1)&&bl_auth.store.remote_count==(changed?0:1));
}
static void same_boot_retry(const char *root){
 char directory[1024];snprintf(directory,sizeof(directory),"%s/same-boot-retry",root);seed(directory,"secret123");
 const char *header="Basic YWRtaW46c2VjcmV0MTIz",*next="retry-password";
 ph_auth_context original;assert(ph_auth_http_check(&bl_auth,header,&original));
 retain_paired_v4();lan_v4_server retained=udp_state.server;unsigned frees=retained_frees;
 uint32_t epoch=ph_auth_credential_epoch(&bl_auth);ph_settings before=bl_device.settings;
 disk_record old_record,saved;assert(read_record((unsigned char *)&old_record,sizeof(old_record))==sizeof(old_record));
 unsigned writes=write_attempts,raw=raw_mutations;
 fail_stage=5;set_password_http(header,next,PH_WIRE_MSGPACK,"storage_error");fail_stage=0;
 assert(write_attempts==writes+1&&raw_mutations==raw&&!strcmp(CFG_GetWebPassword(),"secret123"));
 assert(ph_auth_credential_epoch(&bl_auth)==epoch&&ph_auth_context_valid(&original,PH_AUTH_SCOPE_ADMIN));
 assert(bl_auth.store.count==1&&bl_auth.store.remote_count==1);retained_result(&retained,frees,false);sensor_result(false);
 assert(read_record((unsigned char *)&saved,sizeof(saved))==sizeof(saved)&&!memcmp(&saved,&old_record,sizeof(saved)));
 /* A new id retries the failed transaction without reusing its cached reply. */
 set_password_http_request(header,next,PH_WIRE_MSGPACK,"accepted","port-credential-retry");
 assert(write_attempts==writes+2&&raw_mutations==raw+1&&!strcmp(CFG_GetWebPassword(),next));
 assert(ph_auth_credential_epoch(&bl_auth)==epoch+1&&!ph_auth_context_valid(&original,PH_AUTH_SCOPE_ADMIN));
 assert(!bl_auth.store.count&&!bl_auth.store.remote_count);retained_result(&retained,frees,true);sensor_result(true);preserved_settings(&before);
 assert(read_record((unsigned char *)&saved,sizeof(saved))==sizeof(saved));
 assert(saved.credential_magic==PH_CREDENTIAL_MAGIC&&!strcmp(saved.password,next));
 assert(!saved.settings.auth.count&&!saved.settings.auth.remote_count);
 assert(saved.crc==crc32(&saved.settings,sizeof(saved)-offsetof(disk_record,settings)));
 strcpy(web_password,"secret123");boot();assert(!strcmp(CFG_GetWebPassword(),next));preserved_settings(&before);
}
int main(int argc,char **argv){
 assert(argc==2);assert(!mkdir(argv[1],0700));
 for(unsigned format=PH_WIRE_MSGPACK;format<=PH_WIRE_CBOR;++format){
  for(unsigned mode=0;mode<3;++mode)for(unsigned stage=0;stage<=6;++stage)transaction(argv[1],mode,stage,(ph_wire_format)format);
  empty_password_recovery(argv[1],(ph_wire_format)format);
 }
 for(unsigned stage=0;stage<=6;++stage)sdk_password_transaction(argv[1],stage);
 same_boot_retry(argv[1]);
 ph_lan_v4_revoke(&udp_state);assert(!watched_response);
 ph_status_cache_release(&bl_device);assert(!pthread_mutex_destroy(&mutex));
 printf("S30 actual port journal: %u HTTP matrix + 7 SDK credential transactions, same-boot Basic retry after rename failure, 5 LFS fault stages, OPEN/Basic/Bearer, no-op, SDK-save recovery, empty-password restart, production v4 retained and MQTT SENSOR grant revoke/preservation, settings preservation PASS\n",transactions);
 return 0;
}

static ph_http_request r;
static ph_http_exchange x;
static ph_http_action start_auth(const char*m,const char*p,unsigned n,const char*a){
 memset(&r,0,sizeof(r));strcpy(r.method,m);strcpy(r.path,p);r.have_length=true;r.length=n;
 if(a)strncpy(r.authorization,a,sizeof(r.authorization)-1);memset(&x,0,sizeof(x));x.request=&r;return app.begin(&x);
}
static ph_http_action start(const char*m,const char*p,unsigned n){return start_auth(m,p,n,"Bearer valid");}
static void finish(bool sent){app.release(&x,sent);assert(app.admission()==(job?2:PH_HTTP_CLIENT_SLOTS));assert(!claims&&!waiting&&!wire_live);}
static ph_value parsed(unsigned status){ph_value v;if(x.response.status!=status)fprintf(stderr,"actual %u expected %u\n",x.response.status,status);assert(x.response.status==status);assert(!strcmp(x.response.type,"application/msgpack"));assert(phv_parse(PH_WIRE_MSGPACK,x.response.body,x.response.length,&v));return v;}
static void message(unsigned status,const char*s){ph_value v=parsed(status),t;assert(phv_get(&v,"message",&t)&&phv_equal_text(&t,s));}
static void action(const char*s){ph_value v=parsed(200),t;assert(phv_get(&v,"action",&t)&&phv_equal_text(&t,s));}
static void execute_wire_bytes(const void*wire,size_t n){
 ph_http_server server;wire_input=wire;wire_input_length=n;wire_position=wire_size=0;wire_accepted=wire_closed=false;memset(wire_output,0,sizeof(wire_output));
 assert(ph_http_server_open(&server,&wire_io,&app,3));for(unsigned i=0;i<2000&&!wire_closed;i++)ph_http_server_step(&server);
 assert(wire_closed);wire_server_running=server.running;ph_http_server_close(&server);assert(!claims&&!waiting&&!wire_live);
}
static void execute_wire(const char*s){execute_wire_bytes(s,strlen(s));}
static const unsigned char post[]={0x81,0xa6,'a','c','t','i','o','n',0xaa,'s','t','a','t','u','s','.','g','e','t'};
int main(int argc,char**argv){
 assert(argc==2);unsigned rounds=(unsigned)strtoul(argv[1],NULL,10);assert(rounds&&rounds<=100000);strcpy(password,"pw");assert(MODEL_START()==0);
 execute_wire("GET / HTTP/1.1\r\n\r\n");assert(strstr(wire_output,"Content-Length: 4\r\n"));
 execute_wire("OPTIONS /api/v1/status HTTP/1.1\r\nOrigin: https://mqtt.fjrcn.cn\r\n\r\n");assert(strstr(wire_output,"200 OK")&&strstr(wire_output,"Access-Control-Allow-Origin: https://mqtt.fjrcn.cn"));
 fail_cap=true;execute_wire("POST /api/v1/ota/upload HTTP/1.1\r\nAuthorization: Bearer valid\r\nContent-Length: 3\r\n\r\n123");assert(!ota_begins&&!job);fail_cap=false;aborts=0;
 assert(start("GET","/api/v1/memory",0)==PH_HTTP_REPLY);ph_value mem=parsed(200),data,v,failure,arr,item;int64_t num;
 assert(phv_get(&mem,"data",&data)&&phv_get(&data,"http_core_close_failures",&v));finish(true);
 maximum=true;tick=UINT32_MAX;memory.free_memory=memory.allocted_memory=memory.num_of_chunks=INT32_MAX;
 assert(start("GET","/api/v1/memory",0)==PH_HTTP_REPLY);mem=parsed(200);assert(phv_get(&mem,"data",&data));
 assert(phv_get(&data,"mqtt_ingress_dropped",&v)&&phv_int(&v,&num)&&num==UINT32_MAX);
 assert(phv_get(&data,"mqtt_busy_overflow",&v)&&phv_int(&v,&num)&&num==UINT32_MAX);
 assert(phv_get(&data,"mqtt_failure",&failure)&&phv_get(&failure,"free",&v)&&phv_int(&v,&num)&&num==UINT32_MAX);
 assert(phv_get(&failure,"read",&arr)&&arr.count==4&&phv_at(&arr,0,&item)&&phv_int(&item,&num)&&num==INT32_MIN);finish(true);
 maximum=false;memory.free_memory=50000;fail_diagnostics_alloc=true;assert(start("GET","/api/v1/memory",0)==PH_HTTP_REPLY);assert(x.response.status==503);parsed(503);finish(true);
 const char*paths[]={"/api/v1/info","/api/v1/status","/api/v1/capabilities","/api/v1/config","/api/v1/logs","/api/v1/schedules"};
 for(unsigned i=0;i<6;i++){assert(start("GET",paths[i],0)==PH_HTTP_REPLY);parsed(200);finish(true);}
 deny=true;assert(start("GET","/api/v1/status",0)==PH_HTTP_WAIT);assert(waiting&&!claims);finish(false);
 assert(start("GET","/api/v1/status",0)==PH_HTTP_WAIT);deny=false;assert(app.retry(&x)==PH_HTTP_REPLY);action("status.get");finish(false);
 assert(start("POST","/api/v1/command",sizeof(post))==PH_HTTP_READ_BODY);assert(app.body(&x,post,sizeof(post)));app.complete(&x);action("command");assert(commands==1);finish(true);
 assert(start("POST","/api/v1/command",32)==PH_HTTP_READ_BODY);assert(app.body(&x,post,3));finish(false);
 assert(start("POST","/api/v1/command",sizeof(post))==PH_HTTP_READ_BODY);assert(app.body(&x,post,sizeof(post)));token_valid=false;app.complete(&x);message(401,"unauthorized");assert(commands==1);finish(false);token_valid=true;
 wire_stall=true;execute_wire("POST /api/v1/command HTTP/1.1\r\nAuthorization: Bearer valid\r\nContent-Length: 2\r\n\r\n");wire_stall=false;
 /* Binary command contains NUL, scheduler receives explicit byte length. */
 {unsigned char wire[256];const unsigned char bin[]={0x81,0xa1,'b',0xc4,3,0,0xff,0x80};int n=snprintf((char*)wire,sizeof(wire),"POST /api/v1/command HTTP/1.1\r\nAuthorization: Bearer valid\r\nContent-Length: %zu\r\n\r\n",sizeof(bin));memcpy(wire+n,bin,sizeof(bin));execute_wire_bytes(wire,n+sizeof(bin));assert(strstr(wire_output,"200 OK"));}
 password[0]=0;assert(start("GET","/api/v1/config/backup",0)==PH_HTTP_REPLY);message(403,"set_web_password_first");finish(true);
 assert(start("POST","/api/v1/config/restore",sizeof(backup_bytes))==PH_HTTP_REPLY);message(403,"set_web_password_first");finish(true);assert(!backup_exports&&!backup_imports);strcpy(password,"pw");
 assert(start_auth("GET","/api/v1/config/backup",0,"Bearer bad")==PH_HTTP_REPLY);message(401,"unauthorized");finish(true);
 assert(start_auth("POST","/api/v1/config/restore",sizeof(backup_bytes),"Bearer bad")==PH_HTTP_REPLY);message(401,"unauthorized");finish(true);
 assert(start("GET","/api/v1/config/backup",0)==PH_HTTP_REPLY);assert(x.response.status==200&&!strcmp(x.response.type,"application/octet-stream")&&x.response.length==sizeof(backup_bytes)&&!memcmp(x.response.body,backup_bytes,sizeof(backup_bytes)));finish(true);
 deny=true;assert(start("GET","/api/v1/config/backup",0)==PH_HTTP_WAIT);deny=false;assert(app.retry(&x)==PH_HTTP_REPLY);assert(x.response.status==200);finish(false);
 fail_backup_export=true;assert(start("GET","/api/v1/config/backup",0)==PH_HTTP_REPLY);message(409,"backup_unavailable");finish(true);fail_backup_export=false;
 for(unsigned n=127;n<=129;n+=2){assert(start("POST","/api/v1/config/restore",n)==PH_HTTP_REPLY);message(400,"invalid_backup");finish(true);}
 assert(start("POST","/api/v1/config/restore",128)==PH_HTTP_READ_BODY);assert(app.body(&x,backup_bytes,9));finish(false);assert(!backup_imports);
 deny=true;assert(start("POST","/api/v1/config/restore",128)==PH_HTTP_WAIT);finish(false);deny=false;assert(!backup_imports);
 const char*codes[]={"accepted","busy","storage_error","invalid_backup"};const unsigned statuses[]={202,409,400,400};
 for(unsigned i=0;i<4;i++){backup_import_code=codes[i];assert(start("POST","/api/v1/config/restore",128)==PH_HTTP_READ_BODY);assert(app.body(&x,backup_bytes,9)&&app.body(&x,backup_bytes+9,119));app.complete(&x);message(statuses[i],codes[i]);assert(backup_imports==i+1);finish(i!=0);}
 assert(start("POST","/api/v1/ota/upload",100)==PH_HTTP_WAIT);assert(app.retry(&x)==PH_HTTP_READ_BODY);assert(job&&app.admission()==2);assert(app.body(&x,"123",3));finish(false);assert(!job);aborts=0;
 assert(start("POST","/api/v1/ota/upload-unsigned",3)==PH_HTTP_WAIT);assert(app.retry(&x)==PH_HTTP_READ_BODY);assert(app.body(&x,"123",3));app.complete(&x);assert(x.response.status==202);finish(true);assert(!job);
 MODEL_EXTRA
 assert(start_auth("GET","/",0,"Bearer bad")==PH_HTTP_REPLY);assert(x.response.status==200&&x.response.gzip);finish(true);
 for(unsigned i=0;i<rounds;i++){
  assert(start("GET","/api/v1/status",0)==PH_HTTP_REPLY);action("status.get");finish(true);
  assert(start("POST","/api/v1/command",sizeof(post))==PH_HTTP_READ_BODY);assert(app.body(&x,post,sizeof(post)));app.complete(&x);action("command");finish(true);
  assert(start("POST","/api/v1/command",32)==PH_HTTP_READ_BODY);assert(app.body(&x,post,3));finish(false);
  deny=true;assert(start("GET","/api/v1/status",0)==PH_HTTP_WAIT);deny=false;assert(app.retry(&x)==PH_HTTP_REPLY);action("status.get");finish(false);
  assert(start_auth("GET","/api/v1/status",0,"Bearer bad")==PH_HTTP_REPLY);message(401,"unauthorized");finish(true);
  assert(start("GET","/api/v1/not-found",0)==PH_HTTP_REPLY);assert(x.response.status==404);finish(true);
 }
 MODEL_FINAL
 printf("mixed read-only callback soak: %u rounds; owned MP, binary NUL, backup, restore, OTA, expiry\n",rounds);return 0;
}

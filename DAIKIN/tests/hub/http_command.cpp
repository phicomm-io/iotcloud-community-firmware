/* Production HTTP handlers with an IDF request shim, real common auth and
 * real UDP sockets. The peer business dispatcher is an explicit fixture. */
#include "hub_service.h"
#include "hub_wire.h"
#include "hub_lan_discovery.h"
#include "hub_lan_client.h"
#include "hub_lan_queue.h"
#include "hub_http_command.h"
#include "hub_lan_result.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <openssl/sha.h>
extern "C" {
#include "local_auth.h"
}
#include "lan_client_core.inc"
struct String:std::string {
 using std::string::string;String(const std::string &s):std::string(s){}
 bool startsWith(const char*p)const{return compare(0,strlen(p),p)==0;}
 bool reserve(size_t n){std::string::reserve(n);return true;}
 bool concat(const char*p,size_t n){append(p,n);return true;}
 int indexOf(char c)const{auto n=find(c);return n==npos?-1:int(n);}
 String substring(size_t a,size_t b=npos)const{return substr(a,b==npos?npos:b-a);}
};
using std::min;
#include "wire_mp.h"
struct IPAddress{String address="127.0.0.1";String toString()const{return address;}};
struct WiFiClass{IPAddress localIP(){return {};}IPAddress softAPIP(){IPAddress a;a.address="192.168.0.1";return a;}} WiFi;
using esp_err_t=int;static constexpr int ESP_OK=0,ESP_FAIL=-1;
enum{HTTP_GET,HTTP_POST,HTTP_PUT,HTTP_DELETE,HTTP_OPTIONS};
struct httpd_req_t{
 int method=HTTP_GET,content_len=0;const char*uri="/api/auth/me";
 std::map<std::string,std::string> headers,outHeaders;String input,body,status,type;size_t at=0;
 std::vector<std::pair<const char*,const char*>> responseHeaders;
};
static int httpd_req_get_hdr_value_str(httpd_req_t*r,const char*k,char*out,size_t n){auto i=r->headers.find(k);if(i==r->headers.end()||i->second.size()>=n)return ESP_FAIL;memcpy(out,i->second.c_str(),i->second.size()+1);return ESP_OK;}
static size_t httpd_req_get_hdr_value_len(httpd_req_t*r,const char*k){auto i=r->headers.find(k);return i==r->headers.end()?0:i->second.size();}
static int httpd_req_recv(httpd_req_t*r,char*p,int n){size_t used=std::min(size_t(n),r->input.size()-r->at);memcpy(p,r->input.data()+r->at,used);r->at+=used;return used;}
// Match IDF: field/value are borrowed and consumed only by the send API.
static void httpd_resp_set_hdr(httpd_req_t*r,const char*k,const char*v){r->responseHeaders.emplace_back(k,v);}
static void httpd_resp_set_status(httpd_req_t*r,const char*v){r->status=v;}
static void httpd_resp_set_type(httpd_req_t*r,const char*v){r->type=v;}
static int httpd_resp_send(httpd_req_t*r,const char*p,size_t n){r->outHeaders.clear();for(const auto &header:r->responseHeaders)r->outHeaders[header.first]=header.second;r->responseHeaders.clear();r->body.assign(p?p:"",n);return ESP_OK;}
static uint32_t now=100;static uint32_t millis(){return now;}
static std::function<void()> pump;static void delay(unsigned n){now+=n;if(pump)pump();}
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define DAIKIN_VARIANT "hub"
static String deviceId="02c400000002",deviceHostname="iotcloud-daikin-test";
static DynamicJsonDocument config(16384);static iotcloud::HubService hubService(config);
static bool configRecovery=false,localAuthReady=true,hubRestoring=false,heapReady=true;
static ph_auth_state localAuth;static char webPassword[33]={};static int authMux;
static inline bool localAuthContextValid(const ph_auth_context *c){return c&&c->owner==&localAuth&&!configRecovery&&ph_auth_context_valid(c,PH_AUTH_SCOPE_ADMIN);}
#include "http-auth.inc"
#include "http-globals.inc"
static iotcloud::HubLanDiscovery hubLanDiscovery;
static iotcloud::HubLanClient hubLanClient;static iotcloud::HubLanQueue hubLanQueue;
static iotcloud::HubDeviceBus hubDeviceBus;
static lc_state lanClient;static lc_ops lanOps;
static uint32_t hubLanBoot=81;static bool hubLanRequestStatus=false;static std::string hubLanRequestId;
static const char*hubLanWaitReason="idle";
static void initLanClient(){}
static bool hubReady(){return !hubRestoring&&!configRecovery;}
static size_t byteHeap=256000,byteBlock=128000;
static size_t hubByteHeap(){return heapReady?byteHeap:0;}static size_t hubByteBlock(){return heapReady?byteBlock:0;}
#include "heap-guards.inc"
#include "lan-gates.inc"
#include "lan-start.inc"
#include "lan-failed.inc"
static unsigned localCommands=0;
static String execute(const String &bytes){++localCommands;ph_value root,id;assert(phv_parse(PH_WIRE_MSGPACK,bytes.data(),bytes.size(),&root)&&phv_get(&root,"request_id",&id));char text[65];assert(phv_text(&id,text,sizeof(text)));DynamicJsonDocument result(256);result["request_id"]=text;result["code"]="ok";result["data"]["local"]=true;String wire;serializeWireMP(result,wire);return wire;}
#include "http-command.inc"
#include "hub-api.inc"
static bool businessStatusReady=false;
static String cachedHttpStatus(){return "{}";}static String backupExecute(){return "{}";}
#include "http-api.inc"
static void dispatchHttp(){
#include "http-dispatch.inc"
}
static int serverfd,clientfd;static sockaddr_in serverAddress,clientAddress;
static lan_v4_server server={};static lan_v4_ops ops={};static uint8_t udpToken[16]={1};
static unsigned peerCalls=0,sent=0,states=0;static bool drop=false,revokeAtQueue=false,revokeAtResult=false;
static bool logoutAtQueue=false,logoutAtResult=false,overlapAtQueue=false;static std::string sessionHeader;
static sockaddr_in bindSocket(int &fd){fd=socket(AF_INET,SOCK_DGRAM,0);assert(fd>=0);sockaddr_in a={};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(!bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a)));socklen_t n=sizeof(a);assert(!getsockname(fd,reinterpret_cast<sockaddr*>(&a),&n));return a;}
static bool udp(int fd,const sockaddr_in &a,const void*p,size_t n){return sendto(fd,p,n,0,reinterpret_cast<const sockaddr*>(&a),sizeof(a))==ssize_t(n);}
static std::string pack(const std::string &json){DynamicJsonDocument d(4096);assert(!deserializeJson(d,json));std::string bytes;serializeMsgPack(d,bytes);return bytes;}
static std::string command(const char *action,const std::string &id){return pack("{\"action\":\""+std::string(action)+"\",\"params\":{},\"request_id\":\""+id+"\"}");}
static DynamicJsonDocument decode(const httpd_req_t &r){DynamicJsonDocument out(12288);assert(!deserializeMsgPack(out,r.body));return out;}
static httpd_req_t request(const char *path,int method=HTTP_GET,const std::string &body="",const std::string &auth=""){
 httpd_req_t r;r.uri=path;r.method=method;r.input=body;r.content_len=body.size();r.headers["Host"]="127.0.0.1";r.headers["Content-Type"]="application/msgpack";if(!auth.empty())r.headers["Authorization"]=auth;return r;
}
static void revoke(){assert(ph_auth_credential_commit(&localAuth,"device-password",true,[](void*,const ph_auth_store*){return true;},nullptr));}
static void cycle(){
 dispatchHttp();
 if(overlapAtQueue&&hubHttpDeferred){overlapAtQueue=false;auto other=request("/api/auth/me",HTTP_GET,"",sessionHeader);hubApiHandler(&other);assert(other.status=="503 Service Unavailable");}
 if(logoutAtQueue&&hubHttpDeferred&&hubLanQueue.size()){logoutAtQueue=false;assert(ph_auth_http_logout(&localAuth,sessionHeader.c_str()));}
 if(revokeAtQueue&&hubHttpDeferred&&hubLanQueue.size()){revokeAtQueue=false;revoke();}
 hubLanQueue.pollGuarded(now,hubLanClient.busy(),[](const std::string&r,const std::string&b,const iotcloud::HubLanQueue::Guard&g){return hubLanStart(r,b,false,g);},hubLanFailed);
 char bytes[LAN_V4_FRAME_MAX+1];ssize_t n=recv(serverfd,bytes,sizeof(bytes),MSG_DONTWAIT);
 if(n>0&&!drop){lan_v4_packet p={};assert(lan_v4_decode(bytes,n,&p));char scratch[LAN_V4_FRAME_MAX+1];assert(lan_v4_server_input(&server,&ops,&p,"127.0.0.1",ntohs(clientAddress.sin_port),scratch,sizeof(scratch)));}
 n=recv(clientfd,bytes,sizeof(bytes),MSG_DONTWAIT);
 if(n>0){lan_v4_packet p={};assert(lan_v4_decode(bytes,n,&p));if(logoutAtResult){logoutAtResult=false;assert(ph_auth_http_logout(&localAuth,sessionHeader.c_str()));}if(revokeAtResult){revokeAtResult=false;revoke();}hubLanClient.receive(p,"127.0.0.1",11288,now);}
 hubLanClient.tick(now);pollHubHttpCommand(now);
}
static void registerPeer(const char *model,const char *variant){
 auto &peer=hubLanDiscovery.peers[0];peer.id="02c400000001";peer.model=model;peer.variant=variant;peer.firmware="1.0.0-rc.5-test";peer.ip="127.0.0.1";peer.port=11288;peer.seen=now;
 assert(ph_auth_remote_store(&localAuth,peer.id.c_str(),"01000000000000000000000000000000",PH_AUTH_SCOPE_CONTROL,ph_auth_credential_epoch(&localAuth)));
 auto d=config["hub"]["devices"][0].as<JsonObject>();if(d.isNull())d=config["hub"]["devices"].as<JsonArray>().createNestedObject();d["mac"]=peer.id;d["model"]=model;d["version"]="v2";d["status"]="active";d["username"]="admin";
 config["lan_peers"][0]["peer"]=peer.id;config["lan_peers"][0]["ip"]="127.0.0.1";
 lc_job *job=lc_find(&lanClient,peer.id.c_str(),true);assert(job);strcpy(job->peer,peer.id.c_str());strcpy(job->ip,"127.0.0.1");job->phase=LC_READY;job->epoch=ph_auth_credential_epoch(&localAuth);job->scopes=PH_AUTH_SCOPE_CONTROL;job->ready=now;memcpy(job->token,udpToken,16);
}
static void init(){
 ph_auth_store store;ph_auth_store_empty(&store);ph_auth_ops auth={};auth.now=[](void*){return now;};auth.password=[](void*,char*p){strcpy(p,webPassword);return true;};auth.random=[](void*,uint8_t*p,size_t n){static uint8_t serial=0;for(size_t i=0;i<n;++i)p[i]=++serial;return true;};auth.save=[](void*,const ph_auth_store*){return true;};assert(ph_auth_init(&localAuth,&auth,&store));
 hubService.store=[](DynamicJsonDocument &next){config=std::move(next);return true;};hubService.randomHex=[](size_t n){return std::string(n*2,'a');};assert(hubService.initializeIdentity());
 DynamicJsonDocument roomy(16384);roomy.set(config);config=std::move(roomy);
 assert(ph_auth_remote_store(&localAuth,"02c400000001","01010101010101010101010101010101",1,ph_auth_credential_epoch(&localAuth)));
 lanOps.now=[](){return now;};lanOps.epoch=[](){return ph_auth_credential_epoch(&localAuth);};
 serverAddress=bindSocket(serverfd);clientAddress=bindSocket(clientfd);
 ops.local="02c400000001";ops.boot=91;ops.now=[](void*){return now;};ops.authorize=[](void*,const char*,const uint8_t*,ph_auth_context*c){memset(c,0,sizeof(*c));return true;};ops.valid=[](void*,const ph_auth_context*){return true;};ops.digest=[](void*,const void*p,size_t n,uint8_t*h){return SHA256(static_cast<const unsigned char*>(p),n,h)!=nullptr;};ops.send=[](void*,const char*,uint16_t,const void*p,size_t n){return udp(serverfd,clientAddress,p,n);};
 ops.dispatch=[](void*,const void*p,size_t n,const ph_auth_context*,bool*admitted){*admitted=true;++peerCalls;ph_value root,action,id;char requestId[65];assert(phv_parse(PH_WIRE_MSGPACK,p,n,&root)&&phv_get(&root,"action",&action)&&phv_get(&root,"request_id",&id)&&phv_text(&id,requestId,sizeof(requestId)));ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,nullptr,false);phw_text(&w,"code","ok");phw_text(&w,"request_id",requestId);phw_begin(&w,"data",false);if(phv_equal_text(&action,"capabilities.get"))phw_text(&w,"capability","fixture-full-contract");else phw_text(&w,"snapshot",std::string(3100,'X').c_str());phw_end(&w);phw_end(&w);return phw_take(&w);};
#include "lan-authorize.inc"
 hubLanClient.send=[](const std::string&,uint16_t,const void*p,size_t n){++sent;return udp(clientfd,serverAddress,p,n);};hubLanClient.failed=hubLanFailed;
 hubLanClient.complete=[](const std::string &route,const void*p,size_t n){if(hubHttpCommand.expects(route,hubLanRequestId))hubHttpCommand.complete(route,p,n);iotcloud::routeHubLanResult(hubDeviceBus,route,p,n,hubLanRequestStatus,hubLanRequestId);};
 hubDeviceBus.dispatch=[](const std::string &route,const std::string&,bool,iotcloud::HubDeviceBus::Origin){if(route.size()>6&&route.substr(route.size()-6)=="/state")++states;return true;};pump=cycle;
}
static void receiptBounds(){
 iotcloud::HubHttpCommand pending;const std::string route="route/device/command";
 const char *queries[]={"status.get","capabilities.get","config.get","diagnostics.get","info.get","logs.get",
  "power.get","time.get","binding.status.get","binding.get","night.get","linkage.get","light_effect.get",
  "sensors.average.get","wifi.scan.get","filter.get","alarm.get","wake_alarm.get","mcu.version.get",
  "co2_source.get","co2_display.get","sensor_details.get","music.get","music.read","ota.status.get",
  "ota.check.get","ota.sources.get","ota.notes.get","lan.pair.get","fresh_air.config.get",
  "fresh_air.linkage.get","schedules.get","buttons.get","display_source.get","display_profile.get"};
 std::string parsed;bool readOnly;
 for(const char *action:queries){assert(iotcloud::HubHttpCommand::parse(command(action,"classification"),parsed,&readOnly)&&readOnly);}
 for(const char *action:{"outlet.set","time.set","binding.set","binding.probe","linkage.set","filter.reset",
    "alarm.set","music.begin","music.chunk","music.commit","music.play","music.stop","music.cancel",
    "ota.check","ota.notes","ota.install","ota.cancel","lan.pair","lan.unpair","wifi.scan",
    "co2.calibrate","system.reset","fresh_air.config.set","fresh_air.linkage.set","display_source.set",
    "display_profile.set","display_data.set","schedules.put","schedules.delete","schedules.enable","buttons.set","future.get"}){
   assert(iotcloud::HubHttpCommand::parse(command(action,"classification"),parsed,&readOnly)&&!readOnly);
 }
 auto finish=[&](const std::string&id){auto bytes=pack("{\"code\":\"ok\",\"data\":{},\"request_id\":\""+id+"\"}");assert(pending.complete(route,bytes.data(),bytes.size()));std::string response;int status;assert(pending.take(response,status)&&status==200);};
 for(size_t i=0;i<iotcloud::HubHttpCommand::ReceiptCapacity;++i){const auto id="write-"+std::to_string(i);assert(!pending.begin(route,command("outlet.set",id),[]{return true;},100));finish(id);}
 for(unsigned i=0;i<300;++i){const auto id="read-"+std::to_string(i);assert(!pending.begin(route,command(queries[i%(sizeof(queries)/sizeof(*queries))],id),[]{return true;},101));finish(id);}
 assert(!strcmp(pending.begin(route,command("outlet.set","write-0"),[]{return true;},102),"conflict"));
 assert(!strcmp(pending.begin(route,command("outlet.set","overflow"),[]{return true;},102),"busy"));
 assert(!pending.begin(route,command("outlet.set","after-expiry"),[]{return true;},100+iotcloud::HubHttpCommand::ReceiptLifetime));finish("after-expiry");
 std::string id;auto nul=pack(R"({"action":"status.get","params":{},"request_id":"a\u0000b"})");assert(!iotcloud::HubHttpCommand::parse(nul,id));
 auto duplicate=command("status.get","duplicate");duplicate[0]=char(0x84);duplicate.append("\xa6" "action" "\xaa" "status.get",18);assert(!iotcloud::HubHttpCommand::parse(duplicate,id));
 puts("128 bounded write receipts survive 300 mixed reads across 35 audited queries; mutations/unknown remain protected, full admission busy, expiry reclaims; NUL/duplicate fields rejected PASS");
}
static void heapAdmission(const std::string &bearer){
 const size_t contiguous=PHW_MAX_BYTES+32;
 assert(contiguous>PHW_MAX_BYTES+1&&contiguous>=((PHW_MAX_BYTES+1+15)&~size_t(15)));
 assert(!hubAllocationReady(size_t(-1),1)); // Addition cannot wrap the reserve.
 byteHeap=40000;byteBlock=16500;
 assert(hubAllocationReady(21000,contiguous)&&!hubAllocationReady(21000));
 for(const char *path:{"/api/hub/devices/02c400000002/command","/api/hub/devices/02c400000001/command"}){
  registerPeer("tc1","standard");
  const auto body=command("status.get","fragmented");const size_t budget=body.size()+2*LAN_V4_RESPONSE_MAX+4096;
  // The production HTTP adapter admits both local and UDP commands under
  // fragmentation once the full aggregate budget and largest block exist.
  byteHeap=budget+16384+1;byteBlock=contiguous+1;
  auto pass=request(path,HTTP_POST,body,bearer);hubApiHandler(&pass);assert(pass.status=="200 OK");
  for(unsigned boundary=0;boundary<4;++boundary){
   byteHeap=budget+16384+1;byteBlock=contiguous+1;
   if(boundary<2)byteHeap=budget+16384-boundary;else byteBlock=contiguous-(boundary-2);
   const auto peersBefore=peerCalls,localBefore=localCommands,framesBefore=sent;
   auto rejected=request(path,HTTP_POST,body,bearer);hubApiHandler(&rejected);
   assert(rejected.status=="503 Service Unavailable"&&decode(rejected)["code"]=="busy");
   assert(peerCalls==peersBefore&&localCommands==localBefore&&sent==framesBefore);
  }
 }
 byteHeap=256000;byteBlock=128000;
 puts("HTTP heap admission PASS: aggregate + 16KiB reserve unchanged, NUL/alignment covered, exact free/block boundaries reject before local/UDP execution; fragmented heap accepted; one-argument guard unchanged");
}
int main(){receiptBounds();init();
 // Empty-password access ignores a stale Bearer without occupying a slot.
 for(unsigned i=0;i<8;++i){auto login=request("/api/auth/login",HTTP_POST,pack("{\"password\":\"\"}"));assert(hubApiHandler(&login)==ESP_OK);auto d=decode(login);assert(d["data"]["token"]==""&&d["data"]["password_required"]==false);auto me=request("/api/auth/me",HTTP_GET,"","Bearer stale");hubApiHandler(&me);assert(me.status=="200 OK");}
 for(const auto &slot:localAuth.http)assert(!slot.active);
 strcpy(webPassword,"device-password");assert(ph_auth_credential_commit(&localAuth,webPassword,false,[](void*,const ph_auth_store*){return true;},nullptr));
 auto missing=request("/api/auth/me");hubApiHandler(&missing);assert(missing.status=="401 Unauthorized");
 auto login=request("/api/auth/login",HTTP_POST,pack("{\"username\":\"admin\",\"password\":\"device-password\"}"));hubApiHandler(&login);auto d=decode(login);std::string token=d["data"]["token"]|"";assert(token.size()==32&&d["data"]["password_required"]==true&&login.outHeaders["X-IotCloud-Session"]==token);std::string bearer="Bearer "+token;
 auto me=request("/api/auth/me",HTTP_GET,"",bearer);hubApiHandler(&me);assert(me.status=="200 OK");
 auto basic=request("/api/auth/me",HTTP_GET,"","Basic YWRtaW46ZGV2aWNlLXBhc3N3b3Jk");hubApiHandler(&basic);assert(basic.status=="200 OK");
 auto host=request("/api/auth/me",HTTP_GET,"",bearer);host.headers["Host"]="attacker.invalid";hubApiHandler(&host);assert(host.status=="403 Forbidden");
 heapAdmission(bearer);
 const auto initialStates=states;
 const char *path="/api/hub/devices/02c400000001/command";
 for(const char *model:{"m1","a1","tc1","s30-v1b"}){
  registerPeer(model,"standard");const auto before=peerCalls;auto read=request(path,HTTP_POST,command("status.get",std::string(model)+"-status"),bearer);hubApiHandler(&read);d=decode(read);if(read.status!="200 OK"||peerCalls!=before+1){std::string diagnostic;serializeJson(d,diagnostic);fprintf(stderr,"HTTP read %s status=%s calls=%u sent=%u now=%u body=%s\n",model,read.status.c_str(),peerCalls,sent,now,diagnostic.c_str());}assert(read.status=="200 OK"&&d["request_id"]==std::string(model)+"-status"&&d["data"]["snapshot"].as<std::string>().size()==3100&&peerCalls==before+1);
  auto cap=request(path,HTTP_POST,command("capabilities.get",std::string(model)+"-cap"),bearer);hubApiHandler(&cap);d=decode(cap);assert(cap.status=="200 OK"&&d["data"]["capability"]=="fixture-full-contract");
 }
 assert(states==initialStates+4);
 sessionHeader=bearer;overlapAtQueue=true;auto bounded=request(path,HTTP_POST,command("status.get","overlap"),bearer);hubApiHandler(&bounded);assert(bounded.status=="200 OK"&&!overlapAtQueue);
 auto origin=request(path,HTTP_POST,command("outlet.set","bad-origin"),bearer);origin.headers["Origin"]="http://evil.invalid";const auto before=peerCalls;hubApiHandler(&origin);assert(origin.status=="403 Forbidden"&&peerCalls==before);
 auto write=request(path,HTTP_POST,command("outlet.set","unique-write"),bearer);hubApiHandler(&write);assert(write.status=="200 OK");auto repeat=request(path,HTTP_POST,command("outlet.set","unique-write"),bearer);hubApiHandler(&repeat);assert(repeat.status=="409 Conflict"&&peerCalls==before+1);
 config["lan_peers"][0]["ip"]="127.0.0.2";auto moved=request(path,HTTP_POST,command("status.get","moved"),bearer);hubApiHandler(&moved);assert(moved.status=="409 Conflict");registerPeer("tc1","standard");
 const auto pairCount=localAuth.store.remote_count;const auto epoch=ph_auth_credential_epoch(&localAuth);const auto beforeLogout=peerCalls;
 logoutAtQueue=true;sessionHeader=bearer;auto signedOut=request(path,HTTP_POST,command("outlet.set","logout-queue"),bearer);hubApiHandler(&signedOut);assert(signedOut.status=="401 Unauthorized"&&peerCalls==beforeLogout&&localAuth.store.remote_count==pairCount&&ph_auth_credential_epoch(&localAuth)==epoch);
 login=request("/api/auth/login",HTTP_POST,pack("{\"password\":\"device-password\"}"));hubApiHandler(&login);d=decode(login);bearer="Bearer "+d["data"]["token"].as<std::string>();sessionHeader=bearer;logoutAtResult=true;
 auto signedResult=request(path,HTTP_POST,command("status.get","logout-result"),bearer);hubApiHandler(&signedResult);assert(signedResult.status=="401 Unauthorized"&&localAuth.store.remote_count==pairCount&&ph_auth_credential_epoch(&localAuth)==epoch);
 login=request("/api/auth/login",HTTP_POST,pack("{\"password\":\"device-password\"}"));hubApiHandler(&login);d=decode(login);bearer="Bearer "+d["data"]["token"].as<std::string>();
 revokeAtQueue=true;const auto queuedCalls=peerCalls;auto queued=request(path,HTTP_POST,command("outlet.set","revoked-queue"),bearer);hubApiHandler(&queued);assert(queued.status=="401 Unauthorized"&&peerCalls==queuedCalls);
 // Basic uses the current epoch, so no new Web session is needed after revoke.
 bearer="Basic YWRtaW46ZGV2aWNlLXBhc3N3b3Jk";registerPeer("tc1","standard");revokeAtResult=true;auto result=request(path,HTTP_POST,command("status.get","revoked-result"),bearer);hubApiHandler(&result);assert(result.status=="401 Unauthorized");
 // Loss is terminal for the admitted transaction; idle ticks never replay it.
 registerPeer("tc1","standard");drop=true;auto loss=request(path,HTTP_POST,command("outlet.set","lost-write"),bearer);hubApiHandler(&loss);d=decode(loss);assert(loss.status=="503 Service Unavailable"&&d["request_id"]=="lost-write"&&d["code"]=="transport_error");auto sends=sent;now+=20000;cycle();assert(sent==sends);drop=false;
 auto lossAgain=request(path,HTTP_POST,command("outlet.set","lost-write"),bearer);hubApiHandler(&lossAgain);assert(lossAgain.status=="409 Conflict"&&sent==sends);
 auto logout=request("/api/auth/logout",HTTP_POST,"",bearer);hubApiHandler(&logout);assert(logout.status=="200 OK");
 lan_v4_server_release(&server);close(serverfd);close(clientfd);
 puts("HTTP/common-auth -> guarded Hub queue -> real UDP v4 -> correlated 3100-byte result PASS; M1/A1/TC1/S30 fixture dispatch, capabilities, Host/Origin, open/no slots, 32hex/Basic, queue/result revoke, duplicate write, timeout/no replay");
}

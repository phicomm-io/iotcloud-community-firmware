/* Real localhost UDP: production HubLanClient + shared v4 server + HubHome.
 * Auth and TC1 business operations are fixtures, not password pairing or DUT. */
#include "hub_lan_client.h"
#include "hub_home.h"
#include <cassert>
#include <cstdio>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <openssl/sha.h>
static uint32_t now=10001;
static const char *hub="02c400000002",*device="02c400000001";
static uint8_t token[16]={7};
static int serverfd,clientfd;static sockaddr_in serverAddress,clientAddress;
static unsigned writes=0,queries=0;static bool applied=false,drop=false;
static bool udp(int fd,const sockaddr_in&a,const void*b,size_t n){return sendto(fd,b,n,0,reinterpret_cast<const sockaddr*>(&a),sizeof(a))==ssize_t(n);}
static sockaddr_in bindSocket(int &fd){fd=socket(AF_INET,SOCK_DGRAM,0);assert(fd>=0);sockaddr_in a={};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(!bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a)));socklen_t n=sizeof(a);assert(!getsockname(fd,reinterpret_cast<sockaddr*>(&a),&n));return a;}
static ph_buf *executeFixture(const void *bytes,size_t length){
 DynamicJsonDocument request(2048),reply(2048);assert(!deserializeMsgPack(request,bytes,length));
 reply["request_id"]=request["request_id"];
 if(request["action"]=="outlet.set"){
  assert(request["params"]["index"]==0&&request["params"]["on"].is<bool>());
  ++writes;applied=request["params"]["on"].as<bool>();reply["code"]="accepted";
 }else{
  assert(request["action"]=="status.get");++queries;reply["code"]="ok";
  auto data=reply.createNestedObject("data");data["model"]="tc1";data["device_id"]=device;
  auto states=data.createNestedArray("socket_states");states.add(applied?1:0);for(unsigned i=1;i<6;++i)states.add(0);
 }
 std::string body;serializeMsgPack(reply,body);auto out=ph_buf_new(body.size()+1);assert(out);memcpy(out->buf,body.data(),body.size());out->len=body.size();return out;
}
int main(){
 serverAddress=bindSocket(serverfd);clientAddress=bindSocket(clientfd);
 lan_v4_server server={};lan_v4_ops ops={};ops.local=device;ops.boot=91;
 ops.now=[](void*){return now;};
 ops.authorize=[](void*,const char *peer,const uint8_t incoming[16],ph_auth_context *context){if(strcmp(peer,hub)||memcmp(incoming,token,16))return false;memset(context,0,sizeof(*context));return true;};
 ops.valid=[](void*,const ph_auth_context*){return true;};
 ops.digest=[](void*,const void*p,size_t n,uint8_t hash[32]){return SHA256(static_cast<const unsigned char*>(p),n,hash)!=nullptr;};
 ops.dispatch=[](void*,const void*p,size_t n,const ph_auth_context*,bool *admitted){*admitted=true;return executeFixture(p,n);};
 ops.send=[](void*,const char*,uint16_t,const void*p,size_t n){return drop||udp(serverfd,clientAddress,p,n);};
 iotcloud::HubDeviceBus bus;iotcloud::HubHome home(bus);iotcloud::HubLanClient client;
 const std::string prefix="phicomm/v2/u_66697874757265/",base=prefix+"tc1/"+device;
 DynamicJsonDocument saved(512);auto d=saved.to<JsonArray>().createNestedObject();d["mac"]=device;d["model"]="tc1";
 assert(home.configure(saved.as<JsonArrayConst>(),prefix,hub));
 // Explicit cloud discovery fixture selects MP. No guessed codec or mDNS claim.
 DynamicJsonDocument discovery(2048);discovery["schema"]=2;discovery["protocol"]="tmcp";discovery["protocol_version"]=2;discovery["version"]="v2";discovery["device_id"]=device;discovery["model"]="tc1";discovery["variant"]="standard";discovery["firmware_version"]="1.0.0-rc.4";discovery["wire_version"]=1;discovery["default_wire_format"]="msgpack";discovery.createNestedArray("wire_formats").add("msgpack");
 std::string wire;serializeMsgPack(discovery,wire);iotcloud::hubWire.observe(prefix+"discovery/"+device,wire);assert(iotcloud::hubWire.binary(base));
 unsigned serial=0;home.randomHex=[&](size_t n){auto s=std::to_string(++serial);return std::string(n*2-s.size(),'0')+s;};
 std::vector<std::string> sent;
 client.send=[&](const std::string&ip,uint16_t port,const void*p,size_t n){assert(ip=="127.0.0.1"&&port==ntohs(serverAddress.sin_port));sent.emplace_back(static_cast<const char*>(p),n);return udp(clientfd,serverAddress,p,n);};
 client.complete=[&](const std::string&route,const void*p,size_t n){assert(route==base+"/command");home.tick(now);home.observe(base+"/result",std::string(static_cast<const char*>(p),n));};
 bus.dispatch=[&](const std::string&route,const std::string&body,bool retain,iotcloud::HubDeviceBus::Origin){assert(route==base+"/command"&&!retain);return client.begin(hub,81,device,"127.0.0.1",ntohs(serverAddress.sin_port),token,route,body,now);};
 auto pumpServer=[&](){char frame[1201];auto n=recv(serverfd,frame,1200,MSG_DONTWAIT);if(n<=0)return false;lan_v4_packet p={};assert(lan_v4_decode(frame,size_t(n),&p));char scratch[1201];assert(lan_v4_server_input(&server,&ops,&p,"127.0.0.1",ntohs(clientAddress.sin_port),scratch,sizeof(scratch)));return true;};
 auto pumpClient=[&](){char frame[1201];auto n=recv(clientfd,frame,1200,MSG_DONTWAIT);if(n<=0)return false;lan_v4_packet p={};assert(lan_v4_decode(frame,size_t(n),&p));client.receive(p,"127.0.0.1",ntohs(serverAddress.sin_port),now);return true;};
 auto drain=[&](){for(unsigned i=0;i<100;++i){bool a=pumpServer(),b=pumpClient();if(!a&&!b)break;}};
 // Initial measured state also crosses real UDP; public query ID fixture only.
 home.devices[1].query="initial";DynamicJsonDocument initial(512);initial["action"]="status.get";initial["request_id"]="initial";initial.createNestedObject("params");wire.clear();serializeMsgPack(initial,wire);
 assert(client.begin(hub,81,device,"127.0.0.1",ntohs(serverAddress.sin_port),token,base+"/command",wire,now));drain();assert(queries==1&&home.devices[1].controls&&!home.devices[1].outlets[0]);
 home.tick(now);assert(home.begin(1,iotcloud::HubHome::Outlet,0,1));
 assert(pumpServer());assert(writes==1);assert(pumpClient()); // business ACK starts fresh status request
 assert(!home.finished());assert(home.devices[1].outlets[0]==false);
 // Deliver transport ACK but lose the status response, so ACK alone cannot win.
 assert(pumpServer());drop=true;assert(pumpServer());drop=false;
 assert(queries==2&&!home.finished());now+=401;client.tick(now);drain();
 assert(writes==1&&queries==2&&home.finished()&&home.finish()&&home.devices[1].outlets[0]);
 // Offline model rejects control before any UDP packet is emitted.
 home.observe(base+"/availability","offline");auto count=sent.size();assert(!home.begin(1,iotcloud::HubHome::Outlet,0,0)&&sent.size()==count);
 home.observe(base+"/availability","online");home.devices[1].query="recover";initial["request_id"]="recover";wire.clear();serializeMsgPack(initial,wire);assert(client.begin(hub,81,device,"127.0.0.1",ntohs(serverAddress.sin_port),token,base+"/command",wire,now));drain();
 // Wrong token is rejected by the shared server without a business dispatch.
 uint8_t wrong[16]={99};count=writes;assert(client.begin(hub,81,device,"127.0.0.1",ntohs(serverAddress.sin_port),wrong,base+"/command",wire,now));drain();assert(std::string(client.lastCode)=="auth_required"&&writes==count);
 // Lost mutation responses retry exact IDs/bytes; shared server executes once.
 home.tick(now);drop=true;count=writes;assert(home.begin(1,iotcloud::HubHome::Outlet,0,0));const std::string original=sent.back();drain();assert(writes==count+1&&!home.finished());
 for(unsigned i=0;i<3;++i){now+=401;client.tick(now);assert(sent.back()==original);drain();assert(writes==count+1);}
 now+=LAN_V4_RESPONSE_MS;client.tick(now);assert(!client.busy()&&std::string(client.lastCode)=="timeout"&&!home.finished());
 drop=false;assert(udp(clientfd,serverAddress,original.data(),original.size()));drain();assert(writes==count+1); // no new-ID replay
 lan_v4_server_release(&server);close(serverfd);close(clientfd);
 std::puts("PASS actual UDP v4 HAP model: measured initial state, mutation ACK then fresh status, status loss retry, offline/token rejection, mutation loss no duplicate execution; fixture auth/business, no DUT");
}

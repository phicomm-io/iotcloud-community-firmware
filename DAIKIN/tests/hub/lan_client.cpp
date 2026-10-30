/* Actual localhost UDP, production v4 client and shared server/codec. Auth is
 * a fixed-token fixture; this test does not claim password-pairing acceptance. */
#include "hub_lan_client.h"
#include "hub_lan_queue.h"
#include "hub_lan_result.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <openssl/sha.h>
static uint32_t now=10;static unsigned calls=0;static bool authorized=true,clientAuthorized=true;
static const char *hub="02c400000002",*device="02c400000001";
static uint8_t token[16]={1};static int serverfd,clientfd;static sockaddr_in serverAddress,clientAddress;
static bool udp(int fd,const sockaddr_in &address,const void *bytes,size_t length){return sendto(fd,bytes,length,0,reinterpret_cast<const sockaddr*>(&address),sizeof(address))==ssize_t(length);}
static sockaddr_in bindSocket(int &fd){fd=socket(AF_INET,SOCK_DGRAM,0);assert(fd>=0);sockaddr_in address={};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(!bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)));socklen_t length=sizeof(address);assert(!getsockname(fd,reinterpret_cast<sockaddr*>(&address),&length));return address;}
static ph_buf *response(){ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,nullptr,false);phw_text(&w,"code","ok");phw_text(&w,"request_id","fixture");phw_begin(&w,"data",false);std::string payload(3100,'X');phw_text_n(&w,"payload",payload.data(),payload.size());phw_end(&w);phw_end(&w);return phw_take(&w);}
static void queueTests(){
 iotcloud::HubLanQueue q;std::string source="owned";unsigned starts=0,failures=0;
 assert(q.push("route",source,UINT32_MAX-10));source="mutated";
 assert(q.push("second","second-body",UINT32_MAX-10));assert(!q.push("third","overflow",0));
 auto start=[&](const std::string&r,const std::string&b)->const char*{++starts;assert(r=="route"&&b=="owned");return nullptr;};
 auto fail=[&](const std::string&r,const std::string&b,const char*c){assert(r=="second"&&b=="second-body"&&!strcmp(c,"busy"));++failures;};
 q.poll(20,true,start,fail);assert(q.size()==2&&!starts&&!failures);
 q.poll(20,false,start,fail);assert(q.size()==1&&starts==1);
 q.poll(5000,true,start,fail);assert(q.size()==0&&failures==1);
 assert(q.push("route","owned",6000));
 q.poll(5999,true,start,fail);assert(q.size()==1&&failures==1);
 q.poll(6001,false,[](const std::string&,const std::string&)->const char*{return "busy";},fail);assert(q.size()==1);
 q.clear();assert(!q.size());q.poll(12000,false,start,fail);assert(starts==1&&failures==1);
}
int main(){queueTests();
 serverAddress=bindSocket(serverfd);clientAddress=bindSocket(clientfd);lan_v4_server server={};lan_v4_ops ops={};ops.local=device;ops.boot=91;
 ops.now=[](void*){return now;};ops.authorize=[](void*,const char *peer,const uint8_t incoming[16],ph_auth_context *context){if(!authorized||strcmp(peer,hub)||memcmp(incoming,token,16))return false;memset(context,0,sizeof(*context));return true;};ops.valid=[](void*,const ph_auth_context*){return authorized;};ops.digest=[](void*,const void *data,size_t length,uint8_t hash[32]){return SHA256(static_cast<const unsigned char*>(data),length,hash)!=nullptr;};ops.dispatch=[](void*,const void*,size_t,const ph_auth_context*,bool *admitted){*admitted=true;++calls;return response();};ops.send=[](void*,const char*,uint16_t,const void *bytes,size_t length){return udp(serverfd,clientAddress,bytes,length);};
 iotcloud::HubLanClient client;client.authorize=[](const std::string&,const std::string&,uint16_t,const uint8_t*){return clientAuthorized;};std::vector<std::string> sends;unsigned completed=0;
 client.send=[&](const std::string &ip,uint16_t port,const void *bytes,size_t length){assert(ip=="127.0.0.1"&&port==ntohs(serverAddress.sin_port));sends.emplace_back(static_cast<const char*>(bytes),length);return udp(clientfd,serverAddress,bytes,length);};
 client.complete=[&](const std::string &route,const void *bytes,size_t length){assert(route=="route");ph_value root,code,data,value;assert(phv_parse(PH_WIRE_MSGPACK,bytes,length,&root)&&phv_get(&root,"code",&code)&&phv_equal_text(&code,"ok")&&phv_get(&root,"data",&data)&&phv_get(&data,"payload",&value));assert(value.size==3100&&value.text[100]=='X');++completed;};
 ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,nullptr,false);phw_text(&w,"action","status.get");phw_begin(&w,"params",false);phw_end(&w);phw_text(&w,"request_id","fixture");phw_end(&w);ph_buf *request=phw_take(&w);assert(request);std::string body(request->buf,request->len);ph_buf_free(request);
 auto start=[&](){return client.begin(hub,81,device,"127.0.0.1",ntohs(serverAddress.sin_port),token,"route",body,now);};
 auto pumpServer=[&](bool drop){char frame[1201];ssize_t length=recv(serverfd,frame,1200,MSG_DONTWAIT);if(length<=0)return false;lan_v4_packet p={};assert(lan_v4_decode(frame,size_t(length),&p));if(drop)return true;char scratch[1201];assert(lan_v4_server_input(&server,&ops,&p,"127.0.0.1",ntohs(clientAddress.sin_port),scratch,sizeof(scratch)));return true;};
 auto pumpClient=[&](bool drop){char frame[1201];ssize_t length=recv(clientfd,frame,1200,MSG_DONTWAIT);if(length<=0)return false;lan_v4_packet p={};assert(lan_v4_decode(frame,size_t(length),&p));if(drop)return true;bool consumed=client.receive(p,"127.0.0.1",ntohs(serverAddress.sin_port),now);assert(consumed||!client.busy());return true;};
 auto drain=[&](){for(unsigned i=0;i<100;i++){bool a=pumpServer(false),b=pumpClient(false);if(!a&&!b)break;}};
 assert(start());assert(!start());assert(pumpServer(false));assert(pumpClient(true)); // lose first result
 now+=401;client.tick(now);assert(sends[0]==sends[1]);drain();assert(completed==1&&calls==1&&!client.busy());
 // No write replay after response ACK/release: old exact ID is expired.
 assert(udp(clientfd,serverAddress,sends.front().data(),sends.front().size()));assert(pumpServer(false));char replay[1201];ssize_t count=recv(clientfd,replay,1200,0);lan_v4_packet result={};assert(lan_v4_decode(replay,size_t(count),&result)&&!strcmp(result.code,"expired")&&calls==1);
 assert(start());drain();assert(completed==2&&calls==2);
 // Wrong source port/token/boot cannot satisfy pending response.
 assert(start());assert(pumpServer(false));char frame[1201];count=recv(clientfd,frame,1200,0);assert(lan_v4_decode(frame,size_t(count),&result));assert(!client.receive(result,"127.0.0.1",1,now));result.token[0]^=1;assert(!client.receive(result,"127.0.0.1",ntohs(serverAddress.sin_port),now));client.reset();drain();
 // Stop/reload cancels bytes without rewinding IDs or replaying controls.
 unsigned previous=client.lastId;client.reset();assert(client.lastId==previous);authorized=false;assert(start());drain();assert(client.lastCode==std::string("auth_required"));assert(calls==3);
 // Revoking the current client grant rejects an already queued old receipt,
 // cancels retries, and cannot be bypassed by starting another transaction.
 authorized=true;assert(start());assert(pumpServer(false));unsigned finished=completed;clientAuthorized=false;assert(pumpClient(false));assert(completed==finished&&!client.busy()&&std::string(client.lastCode)=="auth_required");size_t before=sends.size();now+=401;client.tick(now);assert(sends.size()==before&&!start());clientAuthorized=true;
 // Production completion router publishes complete business state only for a
 // matching status query, independently of whether HomeKit is enabled.
 iotcloud::HubDeviceBus bus;unsigned states=0,results=0,available=0;
 bus.dispatch=[&](const std::string &route,const std::string &body,bool retain,iotcloud::HubDeviceBus::Origin origin){assert(!retain&&origin==iotcloud::HubDeviceBus::Origin::Local);if(route=="route/state"){ph_value root,payload;assert(phv_parse(PH_WIRE_MSGPACK,body.data(),body.size(),&root)&&phv_get(&root,"payload",&payload)&&payload.size==3100);++states;}else if(route=="route/result")++results;else if(route=="route/availability"){assert(body=="online");++available;}else assert(false);return true;};
 ph_buf *snapshot=response();assert(snapshot);assert(iotcloud::routeHubLanResult(bus,"route/command",snapshot->buf,snapshot->len,true,"fixture"));assert(states==1&&results==1&&available==1);
 assert(iotcloud::routeHubLanResult(bus,"route/command",snapshot->buf,snapshot->len,false,"fixture"));assert(states==1&&results==2);
 assert(!iotcloud::routeHubLanResult(bus,"route/command",snapshot->buf,snapshot->len,true,"wrong"));assert(states==1&&results==2);ph_buf_free(snapshot);
 // Timeout reports the owned original request exactly once, with no new ID.
 drain();unsigned failures=0;
 client.failed=[&](const std::string&r,const std::string&b,const char*c){assert(r=="route"&&b==body&&!strcmp(c,"timeout"));++failures;};
 assert(start());unsigned timeoutId=client.lastId;now+=LAN_V4_RESPONSE_MS;client.tick(now);assert(failures==1&&!client.busy()&&client.lastId==timeoutId);client.tick(now+5000);assert(failures==1);
 lan_v4_server_release(&server);close(serverfd);close(clientfd);
}

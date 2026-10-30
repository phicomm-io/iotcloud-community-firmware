#include "hub_home.h"
#include "hub_relay.h"
#include <cassert>
using namespace iotcloud;
int main(){
 HubDeviceBus bus;
 assert(!bus.publish("device/command","{}",false,HubDeviceBus::Origin::Internal));
 std::string route,body;bool retained=true;unsigned calls=0;
 bus.dispatch=[&](const std::string &r,const std::string &b,bool retain,HubDeviceBus::Origin){route=r;body=b;retained=retain;++calls;return true;};
 assert(!bus.publish("device/+","{}",false,HubDeviceBus::Origin::Internal));
 assert(!bus.publish("device/command",std::string(8193,'x'),false,HubDeviceBus::Origin::Internal));
 assert(!calls);
 const std::string binary("\x81\xa1x\x00",4);
 assert(bus.publish("device/state",binary,false,HubDeviceBus::Origin::Local));
 assert(calls==1&&body==binary&&!retained);
 // Route keys never establish a session or queue a replay: no handler = fail.
 bus.dispatch=nullptr;
 assert(!bus.publish("device/state",binary,true,HubDeviceBus::Origin::Local));
 DynamicJsonDocument config(4096);HubService service(config);HubRelay relay(config,service,bus);HubHome home(bus);
 const std::string prefix="phicomm/v2/u_6162/",mac="112233445566";
 auto saved=config["hub"].createNestedArray("devices");auto d=saved.createNestedObject();d["mac"]=mac;d["model"]="tc1";
 assert(home.configure(saved,prefix,"aabbccddeeff"));
 home.tick(1000);home.randomHex=[](size_t n){return std::string(n*2,'1');};
 home.observe(prefix+"tc1/"+mac+"/state",R"({"model":"tc1","socket_states":[0,0,0,0,0,0]})");
 auto &tc=home.devices[1];assert(tc.online);tc.controls=true;
 assert(!home.begin(1,HubHome::Outlet,0,1)); // UDP unavailable must fail closed.
 bus.dispatch=[&](const std::string &r,const std::string &b,bool retain,HubDeviceBus::Origin origin){route=r;body=b;retained=retain;assert(origin==HubDeviceBus::Origin::Internal);return true;};
 assert(home.begin(1,HubHome::Outlet,0,1));
 assert(route==prefix+"tc1/"+mac+"/command"&&!retained);
 DynamicJsonDocument request(1024);assert(!deserializeJson(request,body));assert(request["action"]=="outlet.set");
 // External route mapping dispatches once; it does not establish an MQTT client.
 auto bridge=config["hub"].createNestedObject("bridge");bridge["enabled"]=true;
 auto mapping=bridge.createNestedArray("mappings").createNestedObject();mapping["direction"]="in";mapping["remote_topic"]="cloud/input";mapping["local_topic"]="device/command";
 unsigned forwarded=0;bus.dispatch=[&](const std::string &r,const std::string &,bool,HubDeviceBus::Origin origin){assert(r=="device/command"&&origin==HubDeviceBus::Origin::Upstream);++forwarded;return true;};
 relay.incoming("cloud/input","{}",false);assert(forwarded==1);
 relay.incoming("cloud/input","{}",true);assert(forwarded==1);
 assert(!HubDeviceBus::utf8(std::string("\xc0\x80",2)));
 assert(HubDeviceBus::utf8("中文"));
}

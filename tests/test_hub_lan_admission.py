"""Production hub admission must not answer on behalf of unpaired devices."""
import pathlib
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
class HubAdmission(unittest.TestCase):
    def test_owned_routes_and_correlated_overload(self):
        source = (ROOT / 'DAIKIN/src/hub_lan.inc').read_text()
        production = source.split('static bool hubLanSend(', 1)[1].split('static bool hubLanPacket(', 1)[0]
        harness = r'''
#include "hub_lan_queue.h"
#include <vector>
#include <cassert>
struct Peer { std::string model="a1",id="device"; bool registered=true,paired=false,routed=true; };
struct { std::vector<Peer> peers; } hubLanDiscovery;
struct { std::string prefix(){return "local/";} } hubService;
static bool hubLanRegistered(const Peer&p){return p.registered;}
static bool hubLanPaired(const std::string&){return hubLanDiscovery.peers[0].paired;}
static bool hubLanRouteConfirmed(const Peer&p){return p.routed;}
static bool ready=true,otaActive=false,heap=true;
static bool hubReady(){return ready;}
static bool hubAllocationReady(size_t){return heap;}
static uint32_t millis(){return 100;}
static iotcloud::HubLanQueue hubLanQueue;
static unsigned failures=0;
static std::string last;
static void hubLanFailed(const std::string&,const std::string&,const char*c){++failures;last=c;}
static constexpr unsigned LAN_V4_REQUEST_MAX=1024;
'''
        harness += 'static bool hubLanSend(' + production
        harness += r'''
int main(){
 hubLanDiscovery.peers.emplace_back();auto &p=hubLanDiscovery.peers[0];
 assert(!hubLanSend("local/a1/device/command","body",false));assert(!failures&&!hubLanQueue.size());
 p.paired=true;p.registered=false;assert(!hubLanSend("local/a1/device/command","body",false));assert(!failures);
 p.registered=true;p.routed=false;assert(!hubLanSend("local/a1/device/command","body",false));assert(!failures);
 p.routed=true;assert(!hubLanSend("local/a1/other/command","body",false));assert(!failures);
 assert(!hubLanSend("local/a1/device/command","body",true));assert(!failures);
 assert(hubLanSend("local/a1/device/command","body",false));assert(hubLanQueue.size()==1);
 assert(hubLanSend("local/a1/device/command","body",false));assert(hubLanQueue.size()==2);
 assert(!hubLanSend("local/a1/device/command","body",false));assert(failures==1&&last=="busy");
 hubLanQueue.clear();assert(!hubLanSend("local/a1/device/command","",false));assert(failures==2&&last=="invalid_params");
 heap=false;assert(!hubLanSend("local/a1/device/command","body",false));assert(failures==3&&last=="busy");
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p = pathlib.Path(tmp)
            (p/'test.cpp').write_text(harness)
            subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(ROOT/'DAIKIN/include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
if __name__ == '__main__': unittest.main()

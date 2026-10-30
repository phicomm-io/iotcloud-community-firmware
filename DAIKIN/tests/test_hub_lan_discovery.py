"""Discovery contract validation; no mDNS advertisement authorizes control."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class Discovery(unittest.TestCase):
 def test_explicit_capability_only(self):
  with tempfile.TemporaryDirectory() as directory:
   p=Path(directory);(p/'test.cpp').write_text(r'''
#include "hub_lan_discovery.h"
#include <cassert>
int main(){iotcloud::HubLanDiscovery r;std::map<std::string,std::string> t{{"id","02c400000001"},{"model","tc1"},{"variant","standard"},{"firmware","1.0.0-rc.4"},{"lan_wire","4"},{"codec","msgpack"}};
assert(r.observe(t,"10.0.0.2",11288,10));assert(r.find(t["id"],11));assert(!r.find(t["id"],90010));
for(const char *k:{"id","model","variant","firmware","lan_wire","codec"}){auto bad=t;bad.erase(k);assert(!r.observe(bad,"10.0.0.2",11288,20));}
auto bad=t;bad["lan_wire"]="3";assert(!r.observe(bad,"10.0.0.2",11288,20));bad=t;bad["codec"]="json";assert(!r.observe(bad,"10.0.0.2",11288,20));bad=t;bad["id"]="ffffffffffff";assert(!r.observe(bad,"10.0.0.2",11288,20));
r.clear();assert(!r.find(t["id"],20));
}
''')
   subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(ROOT/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)

"""Real ArduinoJson codec and hub format selection regressions."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
class Wire(unittest.TestCase):
 def test_discovery_and_binary_boundaries(self):
  with tempfile.TemporaryDirectory() as directory:
   temp=Path(directory)
   source=temp/'wire.cpp'
   source.write_text(r'''
#include "hub_wire.h"
#include <cassert>
using namespace iotcloud;
int main(){
 const std::string prefix="phicomm/v2/u_6162/", mac="001122334455",base=prefix+"a1/"+mac,topic=prefix+"discovery/"+mac;
 HubWire wire;DynamicJsonDocument doc(2048),parsed(2048);
 doc["schema"]=1;doc["model"]="a1";doc["protocol"]="tmcp";doc["protocol_version"]=2;
 std::string body;serializeJson(doc,body);wire.observe(topic,body);assert(!wire.binary(base));
 doc["schema"]=2;doc["version"]="v2";doc["firmware_version"]="1.0-rc.4";doc["variant"]="standard";doc["device_id"]=mac;doc["wire_version"]=1;doc["default_wire_format"]="msgpack";doc.createNestedArray("wire_formats").add("msgpack");
 body.clear();serializeMsgPack(doc,body);wire.observe(topic,body);assert(wire.binary(base));
 doc.clear();doc["zero"]=0;doc["params"]["enabled"]=false;
 body.clear();wire.encode(doc,base,body);assert(body.find('\0')!=std::string::npos);assert(!wire.decode(parsed,base,body));assert(parsed["zero"]==0&&parsed["params"]["enabled"]==false);
 std::string json;serializeJson(doc,json);assert(wire.decode(parsed,base,json)); // no per-payload fallback
 assert(!wire.decode(parsed,prefix+"m1/"+mac,json)); // old device preserved
 doc.clear();doc["schema"]=2;doc["model"]="a1";doc["protocol"]="tmcp";doc["protocol_version"]=2;doc["device_id"]="ffffffffffff";doc["wire_version"]=1;doc["default_wire_format"]="json";
 body.clear();serializeMsgPack(doc,body);wire.observe(topic,body);assert(wire.binary(base)); // invalid discovery cannot downgrade
}
''')
   subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(ROOT/'include'),'-I'+str(ROOT/'.pio/libdeps/bry88ab-hub/ArduinoJson/src'),str(source),'-o',str(temp/'wire')],check=True)
   subprocess.run([str(temp/'wire')],check=True)

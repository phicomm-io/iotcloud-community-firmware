"""Compile production button pump and HAP wait; physical IO/HAP are explicit stubs."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
def function(path,start):
 text=path.read_text();begin=text.index(start);brace=text.index('{',begin);depth=1;end=brace+1
 while depth:
  if text[end]=='{':depth+=1
  elif text[end]=='}':depth-=1
  end+=1
 return text[begin:end]
class LocalControlPriority(unittest.TestCase):
 def test_button_and_sensor_pumps_during_actual_hap_wait(self):
  local=function(ROOT/'src/main.cpp','static void pollLocalControls(uint32_t now){')
  home=function(ROOT/'src/home.inc','static bool homeAwaitControl(){')+'\n'+function(ROOT/'src/home.inc','static bool homeControl(size_t device,')
  source=r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
static uint32_t clockMs=1000;static bool down=true,otaActive=false;
static unsigned resets=0,sensors=0,indicators=0,caches=0,network=0,finishes=0;
static uint32_t resetAt=0;
static uint32_t millis(){return clockMs;}
static const int LOW=0;
static int digitalRead(int pin){assert(pin==34);return down?LOW:1;}
static void startWifi(){++resets;resetAt=clockMs;}
static void pollSensors(uint32_t now){assert(now==clockMs);++sensors;}
static void pollIndicators(uint32_t now){assert(now==clockMs);++indicators;}
static void pollBusinessStatusCache(uint32_t now){assert(now==clockMs);++caches;}
static void delay(unsigned ms){clockMs+=ms;}
namespace iotcloud {struct HubHome{enum Control{Outlet};};}
static bool homeHealthy(){return true;}
struct {
 void tick(uint32_t){}
 bool begin(size_t,iotcloud::HubHome::Control,unsigned,double,bool,bool){return true;}
 bool finished(){return false;}
 bool finish(){++finishes;return false;}
}hubHome;
static void pollHubLan(uint32_t now){assert(now==clockMs);++network;}
'''+local+'\n'+home+r'''
int main(){
 pollLocalControls(clockMs); // Physical key began before the HAP transaction.
 clockMs=5000;assert(!homeControl(0,iotcloud::HubHome::Outlet,0,1));
 assert(resets==1&&resetAt==6000&&clockMs==7500);
 assert(sensors==2501&&caches==2501&&indicators==2501&&network==2500&&finishes==1);
 pollLocalControls(clockMs);assert(resets==1); // Still held: no second reset.
 down=false;clockMs=7600;pollLocalControls(clockMs);
 down=true;clockMs=8000;pollLocalControls(clockMs);
 clockMs=11000;assert(!homeControl(0,iotcloud::HubHome::Outlet,0,1));
 assert(resets==2&&resetAt==13000&&clockMs==13500);
 unsigned oldSensors=sensors;otaActive=true;clockMs=14000;pollLocalControls(clockMs);
 assert(sensors==oldSensors); // OTA still pumps indicators/cache, never sensor I/O.
 assert(caches==indicators);
}
'''
  with tempfile.TemporaryDirectory() as directory:
   temp=Path(directory);(temp/'local.cpp').write_text(source)
   subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(temp/'local.cpp'),'-o',str(temp/'local')],check=True)
   subprocess.run([str(temp/'local')],check=True,timeout=10)

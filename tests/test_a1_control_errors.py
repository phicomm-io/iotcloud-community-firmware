"""Production native control parser: invalid input never becomes busy."""
from pathlib import Path
import subprocess, tempfile, unittest
from tests.test_runtime_mqtt_mp import function
ROOT=Path(__file__).resolve().parents[1]
class A1ControlErrors(unittest.TestCase):
 def test_invalid_input_and_busy_are_distinct(self):
  source=(ROOT/'common/service_product_mp.inc').read_text()
  types=(ROOT/'A1/a1.h').read_text();types=types[types.index('enum { A1_CONTROL_FAN'):types.index('bool a1_control_apply(')]
  code='''#include <assert.h>
#include <string.h>
#include "typed_wire.h"
'''+types+'''static unsigned calls;static bool ready=true;static a1_control_t last;
static bool a1_control_apply(a1_control_t*r,bool button,bool broadcast){assert(!button&&broadcast);++calls;last=*r;return ready;}
'''
  code+='\n'.join(function(source,n) for n in ('static bool smp_has(', 'static bool smp_int(', 'static const char *smp_control('))
  code+='''
static void run(const char *action,const unsigned char *bytes,size_t n,const char*expect,bool applied){ph_value p;assert(phv_parse(PH_WIRE_MSGPACK,bytes,n,&p));unsigned before=calls;assert(!strcmp(smp_control(action,&p),expect));assert(calls==before+applied);}
#define RUN(action,expect,applied,...) do{const unsigned char bytes[]={__VA_ARGS__};run(action,bytes,sizeof(bytes),expect,applied);}while(0)
int main(void){
 RUN("fan.set","invalid_params",false,0x81,0xa5,'s','p','e','e','d',101);
 RUN("fan.set","invalid_params",false,0x81,0xa5,'s','p','e','e','d',0xff);
 RUN("fan.set","invalid_params",false,0x81,0xa2,'o','n',1);
 RUN("fan.set","invalid_params",false,0x80);
 RUN("fan.set","invalid_params",false,0x81,0xa1,'x',0xc3);
 RUN("fan.set","invalid_params",false,0x81,0xa5,'s','p','e','e','d',0xca,0x42,0x48,0,0);
 RUN("light.set","invalid_params",false,0x81,0xa5,'c','o','l','o','r',0x81,0xa1,'r',1);
 RUN("fan.set","accepted",true,0x81,0xa5,'s','p','e','e','d',50);assert(last.speed==50&&last.fields==A1_SET_SPEED);
 ready=false;RUN("fan.set","busy",true,0x81,0xa5,'s','p','e','e','d',50);
 ready=true;RUN("light.set","accepted",true,0x81,0xaa,'b','r','i','g','h','t','n','e','s','s',100);assert(last.brightness==255);
 return 0;}
'''
  with tempfile.TemporaryDirectory() as d:
   p=Path(d);(p/'test.c').write_text(code)
   subprocess.run(['cc','-std=c99','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(ROOT/'common'),str(p/'test.c'),str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c'),'-lm','-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)

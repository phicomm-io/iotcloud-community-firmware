"""S30 production publisher -> unchanged M1 display consumer; no JSON linked."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from TC1.tests.test_power_sensor import function
from tests.test_m1_display_mp import DisplayMPTest

ROOT=Path(__file__).resolve().parents[1]


class S30SensorMP(unittest.TestCase):
    def test_publisher_retry_lease_and_unchanged_m1_consumer(self):
        import msgpack
        with tempfile.TemporaryDirectory(prefix='s30-m1-sensors-') as temporary:
            out=Path(temporary)
            (out/'sensor-cycle.inc').write_text(function((ROOT/'BL2028N/src/ph_mqtt_port.c').read_text(),'static int sensor_cycle('))
            flags=[os.environ.get('CC','cc'),'-std=gnu99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(ROOT/'common'),'-I'+str(out)]
            sources=[ROOT/'BL2028N/tests/sensor.c']+[ROOT/'BL2028N/src'/f'ph_{n}.c' for n in ('device','meter','wire')]+[ROOT/'BL2028N/protocol/local_auth.c']
            exe=out/'producer';subprocess.run(flags+list(map(str,sources))+['-lm','-o',str(exe)],check=True)
            result=subprocess.run([str(exe)],capture_output=True,text=True,check=True,timeout=15)
            samples=[];metadata=[]
            for line in result.stdout.splitlines():
                retained,topic,hexbytes=line.split('\t');body=msgpack.unpackb(bytes.fromhex(hexbytes),raw=False)
                self.assertEqual(body['schema'],3);self.assertEqual(body['device_id'],'aabbccddeeff')
                if retained=='1':
                    self.assertTrue(topic.endswith('/metadata'));metadata.append((body,hexbytes))
                else:
                    self.assertTrue(topic.endswith('/state'));samples.append((body,hexbytes))
            self.assertEqual(len(metadata),2)
            self.assertEqual({k:v['unit'] for k,v in metadata[0][0]['fields'].items()},dict(voltage='V',current='A',power='W',energy='kWh'))
            self.assertEqual([b['sequence'] for b,_ in samples],[1,2,3,4,5,1])
            self.assertEqual({b['boot'] for b,_ in samples},{99})
            for field in samples[0][0]['sensors'].values():self.assertFalse(field['valid']);self.assertIsNone(field['value'])
            self.assertTrue(samples[1][0]['sensors']['power']['valid'])
            self.assertGreater(samples[1][0]['sensors']['power']['value'],100)
            self.assertFalse(samples[2][0]['sensors']['power']['valid'])
            self.assertEqual(samples[2][0]['sensors']['power']['age_ms'],5000)
            self.assertEqual(samples[3][0]['sensors']['power']['value'],0)
            # Reuse the complete, already compiled M1 fixture's product stubs;
            # only the scenario main is changed. No M1 consumer implementation copy.
            import inspect,re
            source=inspect.getsource(DisplayMPTest.test_typed_commands_and_sensor_recovery)
            source=re.search("source=r'''(.*?)'''",source,re.S).group(1)
            source=source[:source.index('int main(void){')]
            source+=r'''
static void ingest(const char *hex,bool meta){
 unsigned char bytes[2048];size_t n=strlen(hex)/2;assert(n<sizeof(bytes));
 for(size_t i=0;i<n;i++){unsigned b;assert(sscanf(hex+2*i,"%2x",&b)==1);bytes[i]=b;}
 ph_value v;assert(phv_parse(PH_WIRE_MSGPACK,bytes,n,&v));
 if(meta)m1_display_source_metadata("aabbccddeeff",&v);else m1_display_source_sample("aabbccddeeff",&v);
 memset(bytes,0,sizeof(bytes));
}
int main(int argc,char **argv){
 assert(argc==5);m1_display_defaults(&live);live.configured=1;live.reserved=M1_DISPLAY_FORMATTED;live.revision=1;
 live.lanes[0]=(m1_display_lane_config_t){.source=M1_DISPLAY_MQTT,.metric=M1_DISPLAY_CUSTOM,.unit=M1_DISPLAY_UNITLESS,.ttl_seconds=20,.scale=1};
 strcpy(live.lanes[0].device_id,"aabbccddeeff");strcpy(live.remote[0].field_id,"power");strcpy(live.remote[0].unit,"W");live.style[0].mode=M1_DISPLAY_INTEGER;
 ingest(argv[2],false);assert(!readvalue().valid);ingest(argv[1],true);
 ingest(argv[2],false);assert(readvalue().valid&&readvalue().number>100);
 ingest(argv[3],false);assert(!readvalue().valid); // S30 age expires before M1's 20-second lease.
 ingest(argv[4],false);assert(readvalue().valid&&readvalue().number==0);
 now+=20000;assert(!readvalue().valid);assert(!saves);return 0;
}
'''
            src=out/'consumer.c';src.write_text(source);exe=out/'consumer'
            # Fixture headers cover the SDK declarations; no JSON library links.
            subprocess.run(flags+['-Wno-unused-function','-I'+str(ROOT),'-I'+str(ROOT/'tests/host'),str(src),str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c'),'-lm','-o',str(exe)],check=True)
            subprocess.run([str(exe),metadata[0][1],samples[1][1],samples[2][1],samples[3][1]],check=True,timeout=15)


if __name__=='__main__':unittest.main()

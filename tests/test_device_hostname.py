"""DHCP/mDNS names: factory identity, exact capacity and retained SDK pointer."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DeviceHostname(unittest.TestCase):
    def test_identity_and_buffer_boundaries(self):
        source = r'''
#include <assert.h>
#include <stdio.h>
#include "common/device_hostname_mico.h"
static char *retained;
#include <unistd.h>
char *mock_mico_hostname(char *name) __asm__("sethostname");
char *mock_mico_hostname(char *name){retained=name;return name;}
int main(void){
 char name[64];
 assert(ph_device_hostname(name,sizeof name,"tc1","AABBCCDDEEFF"));
 assert(!strcmp(name,"iotcloud-tc1-ddeeff"));
 size_t n=strlen(name);
 for(size_t cap=1;cap<=n+1;cap++){
  char guarded[66];memset(guarded,0x5a,sizeof guarded);
  assert(ph_device_hostname(guarded+1,cap,"tc1","aabbccddeeff")== (cap==n+1));
  assert(guarded[0]==0x5a&&guarded[cap+1]==0x5a);
 }
 const char *bad[]={"000000000000","ffffffffffff","01bbccddeeff","aabbcc000000","aabbccddeefg","aabbccddeef","aabbccddeeff0"};
 for(unsigned i=0;i<sizeof bad/sizeof *bad;i++)assert(!ph_device_hostname(name,sizeof name,"a1",bad[i]));
 const char *models[]={"","A1","a1.","-a1","a1-","a1/else","a1 name"};
 for(unsigned i=0;i<sizeof models/sizeof *models;i++)assert(!ph_device_hostname(name,sizeof name,models[i],"aabbccddeeff"));
 assert(ph_device_hostname_mico("m1","aabbccddeeff"));
 assert(retained&&!strcmp(retained,"iotcloud-m1-ddeeff"));
 for(unsigned i=0;i<100;i++){volatile char overwrite[1000];memset((void*)overwrite,0,sizeof overwrite);}
 assert(!strcmp(retained,"iotcloud-m1-ddeeff"));
 puts("hostname factory validation, exact capacity and retained lifetime PASS");
}
'''
        with tempfile.TemporaryDirectory(prefix='phicomm-hostname-') as directory:
            base=Path(directory);(base/'case.c').write_text(source)
            subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',
                            '-I'+str(ROOT),str(base/'case.c'),'-o',str(base/'case')],check=True)
            subprocess.run([str(base/'case')],check=True)


if __name__ == '__main__':
    unittest.main()

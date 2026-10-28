"""Execute the actual firmware reader against publisher bytes and hostile inputs."""
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = Path(os.environ.get('NATIVE_MP_ROOT', HERE.parent))

def pack(v):
    # Independent test encoder; golden bytes come from tools/release/device_manifest.py.
    if isinstance(v, str):
        data = v.encode()
        return (bytes([0xa0 | len(data)]) if len(data) < 32 else b'\xda' + struct.pack('>H', len(data))) + data
    if type(v) is bool:
        return b'\xc3' if v else b'\xc2'
    if type(v) is int:
        return b'\xd3' + struct.pack('>q', v)
    if type(v) is float:
        return b'\xcb' + struct.pack('>d', v)
    if isinstance(v, dict):
        return bytes([0x80 | len(v)]) + b''.join(pack(k) + pack(value) for k, value in v.items())
    raise TypeError(v)

class ReleaseManifestMP(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='release-reader-mp-')
        path = Path(cls.temp.name)
        source = path/'case.c'
        source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "common/ota/release_manifest.h"
int main(int argc,char **argv) {
 unsigned char bytes[20000];size_t length=fread(bytes,1,sizeof(bytes),stdin);
 ph_release_manifest out;assert(argc==3);
 bool valid=ph_release_manifest_parse(bytes,length,argv[1],argv[2],&out);
 memset(bytes,0,sizeof(bytes));
 if(valid)printf("%s\n%s\n%s\n%u\n",out.version,out.path,out.release_log,out.size);
 return valid?0:2;
}
''')
        cmd = shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-g','-I'+str(ROOT)]
        if os.environ.get('SANITIZE') == '1':
            cmd += ['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
        cls.exe = path/'case'
        result = subprocess.run(cmd+[str(source),str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c'),'-lm','-o',str(cls.exe)],capture_output=True,text=True)
        if result.returncode: raise AssertionError(result.stdout+result.stderr)
        cls.golden = json.loads((HERE/'release_manifest_mp/publisher.json').read_text())
    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()
    def check(self, data, valid, model='A1', variant='full'):
        result = subprocess.run([str(self.exe),model,variant],input=data,capture_output=True,timeout=10)
        self.assertEqual(result.returncode,0 if valid else 2,result.stderr.decode(errors='replace'))
        return result.stdout.decode()
    def test_publisher_golden_and_owned_output(self):
        result = self.check((HERE/'release_manifest_mp/publisher.msgpack').read_bytes(),True)
        self.assertEqual(result,self.golden['version']+'\n'+self.golden['path']+'\n'+self.golden['release_log']+'\n378120\n')
        for model in ['A1','M1','TC1']:
            for variant in ['full','core','sensor-bme280']:
                doc = dict(self.golden,model=model,firmware_variant=variant)
                self.check(pack(doc),True,model,variant)
        self.check(pack(self.golden),False,'M1');self.check(pack(self.golden),False,variant='core')
    def test_required_fields_types_and_limits(self):
        for key in self.golden:
            with self.subTest(missing=key):
                doc=dict(self.golden);del doc[key];self.check(pack(doc),False)
            with self.subTest(type=key):self.check(pack(dict(self.golden,**{key:True})),False)
        for size in [1,4294967295]:self.check(pack(dict(self.golden,size=size)),True)
        for size in [0,-1,4294967296,1.0,'1']:self.check(pack(dict(self.golden,size=size)),False)
        for key,value in [('schema',2),('schema','1'),('sha256','0'*63),('sha256','g'*64),('commit','0'*39),('commit','z'*40),('tag','1.0.0-rc.4'),('tag','v1.0.0')]:
            self.check(pack(dict(self.golden,**{key:value})),False)
        self.check(pack(dict(self.golden,unknown=1)),False)
    def test_versions_and_path_traversal(self):
        for version in ['1.0.0','1.0.0-rc','1.0.0-rc.4','1.0.0+build.2']:
            self.check(pack(dict(self.golden,version=version,tag='v'+version)),True)
        for version in ['01.0.0','1.0','1.0.0-beta','1.0.0-rc.01','1.0.0+','4294967296.0.0']:
            self.check(pack(dict(self.golden,version=version,tag='v'+version)),False)
        for key in ['path','release_log']:
            for path in ['https://host/releases/v1/a.signed','/releases/v1/../a.signed','/releases/v1/%2e%2e/a.signed','/releases/v1/./../a.signed','/releases/v1/a?x.signed','/releases/v1/a\\b.signed','/other/v1/a.signed','/releases/v1/'+'a'*200+'.signed']:
                self.check(pack(dict(self.golden,**{key:path})),False)
        self.check(pack(dict(self.golden,path='/releases/v1/artifact.bin')),False)
    def test_duplicate_nul_truncation_and_oversize(self):
        data=pack(self.golden)
        duplicate=bytes([data[0]+1])+data[1:]+pack('schema')+pack(1)
        self.check(duplicate,False)
        for key in ['model','firmware_variant','tag','version','path','release_log','sha256','commit']:
            self.check(pack(dict(self.golden,**{key:self.golden[key]+'\0'})),False)
        for cut in range(len(data)):self.check(data[:cut],False)
        self.check(data+b'\xc0',False)
        self.check(pack(dict(self.golden,release_log='a'*9000)),False)

if __name__ == '__main__': unittest.main()

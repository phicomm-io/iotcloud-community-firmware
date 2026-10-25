"""SDK-compatible host RNG status, including partial and failed reads."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class HostRng(unittest.TestCase):
    def test_rng_sdk_signature_and_full_read_requirement(self):
        source = r'''
#include "tests/host/mico.h"
static int mode;
static unsigned calls;
ssize_t __wrap_getrandom(void *buffer,size_t bytes,unsigned flags){
    calls++;assert(!flags);
    if(mode==2)return -1;
    size_t n=mode==1&&bytes?bytes-1:bytes;
    if(n)memset(buffer,0xa5,n);
    return (ssize_t)n;
}
int main(void){
    OSStatus (*sdk_rng)(void *,int)=MicoRandomNumberRead;
    uint8_t bytes[4]={0};
    assert(sdk_rng(bytes,sizeof(bytes))==kNoErr&&calls==1);
    for(unsigned i=0;i<sizeof(bytes);i++)assert(bytes[i]==0xa5);
    mode=1;assert(sdk_rng(bytes,sizeof(bytes))!=kNoErr&&calls==2);
    mode=2;assert(sdk_rng(bytes,sizeof(bytes))!=kNoErr&&calls==3);
    assert(sdk_rng(bytes,-1)==kParamErr&&calls==3);
    assert(sdk_rng(NULL,4)==kParamErr&&calls==3);
    mode=0;assert(sdk_rng(NULL,0)==kNoErr&&calls==4);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'rng.c').write_text(source)
            subprocess.run([os.environ.get('CC', 'cc'), '-D_GNU_SOURCE', '-std=c99',
                '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
                '-fno-pie', '-no-pie', '-I' + str(ROOT), str(path / 'rng.c'),
                '-Wl,--wrap=getrandom', '-o', str(path / 'rng')], check=True, timeout=30)
            subprocess.run([str(path / 'rng')], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()

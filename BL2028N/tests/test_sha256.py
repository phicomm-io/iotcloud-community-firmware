#!/usr/bin/env python3
"""Production fixed-stack SDK SHA256, known vectors and binary differentials."""
import argparse
import hashlib
import json
import random
import shutil
import subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
HERE=Path(__file__).resolve().parent
SDK=Path.home()/'.cache/phicomm/openbeken/bk7231n-upstream'
WOLF=SDK/'platforms/bk7231n/bk7231n_os/beken378/func/wolfssl'

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--run-dir',type=Path,required=True);a=p.parse_args();run=a.run_dir.resolve();run.mkdir(parents=True,exist_ok=True)
 revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=SDK,text=True).strip()
 assert revision=='07fd9e0e2da0aabaddab00be2a897589be235158',revision
 # The host supplies no chipset/TLS configuration. Production uses its pinned
 # SDK configuration and independently compiles the same no-allocation guard.
 (run/'sys_config.h').write_text('/* Host SHA256-only SDK config facade. */\n')
 flags=[shutil.which('clang') or 'cc','-std=c99','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-DPH_S30_LAN_V4','-I',str(run),'-I',str(WOLF)]
 cmd=flags+[str(HERE/'sha256.c'),str(ROOT/'BL2028N/src/ph_sha256.c'),'-Wl,--wrap=malloc,--wrap=realloc,--wrap=free','-o',str(run/'sha256')]
 (run/'command.json').write_text(json.dumps(cmd,indent=2)+'\n')
 with (run/'compiler.log').open('w') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
 rng=random.Random(0x5304)
 vectors=[b'',b'abc',b'abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq',bytes(range(256)),b'\0\xff\0',b'a'*55,b'a'*56,b'a'*63,b'a'*64,b'a'*65]
 vectors += [bytes(rng.randrange(256) for _ in range(n)) for n in [1,2,7,31,32,54,57,62,66,127,128,129,511,512,513,1199,1200,4095,4096]]
 vectors += [bytes(rng.randrange(256) for _ in range(rng.randrange(4097))) for _ in range(100)]
 text=''.join(v.hex()+'\n' for v in vectors)+'million\n'
 with (run/'stderr.log').open('w') as log:r=subprocess.run([str(run/'sha256')],input=text,text=True,stdout=subprocess.PIPE,stderr=log,check=True)
 expected=[hashlib.sha256(v).hexdigest() for v in vectors]+[hashlib.sha256(b'a'*1000000).hexdigest()]
 assert r.stdout.splitlines()==expected
 report={'sdk_commit':revision,'vectors':len(expected),'heap_allocations':0,'sources':{str(p.relative_to(SDK)):hashlib.sha256(p.read_bytes()).hexdigest() for p in (WOLF/'wolfcrypt/src/sha256.c',WOLF/'wolfcrypt/src/misc.c',WOLF/'wolfssl/wolfcrypt/settings.h')}}
 (run/'result.json').write_text(json.dumps(report,indent=2)+'\n')
 print(f'PASS: {len(expected)} SDK SHA256 vectors including binary NUL, block/padding boundaries and million-a; zero allocator calls')
if __name__=='__main__':main()

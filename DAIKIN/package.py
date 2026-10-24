#!/usr/bin/env python3
"""Package the universal ESP32 image; factory images are flashed at address 0."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

project = Path(__file__).resolve().parent
root = project.parent
out = root / 'build/daikin-release'
out.mkdir(parents=True, exist_ok=True)
for stale in out.glob('daikin-bry88ab-*.bin'): stale.unlink()
core = Path(os.environ.get('PLATFORMIO_CORE_DIR', Path.home() / '.platformio'))
tool = core / 'packages/tool-esptoolpy/esptool.py'
framework = core / 'packages/framework-arduinoespressif32'
manifest = {'hardware_model':'BRY88AX151K','flash_size':4194304,'factory_offset':0,'variants':{}}
for variant in ('auto',):
    name = 'bry88ab'
    build = project / '.pio/build' / name
    app = (build / 'firmware.bin').read_bytes()
    target = b'BRY88AB151K'.ljust(32,b'\0')
    if app[288:320] != target:
        raise RuntimeError('Missing or misplaced OTA model descriptor: ' + name)
    base = out / ('daikin-' + name)
    application = base.with_suffix('.application.bin')
    shutil.copyfile(build / 'firmware.bin', application)
    factory = base.with_suffix('.factory.bin')
    subprocess.run([sys.executable,str(tool),'--chip','esp32','merge_bin',
        '--flash_mode','dio','--flash_freq','40m','--flash_size','4MB','-o',str(factory),
        '0x1000',str(build/'bootloader.bin'),'0x8000',str(build/'partitions.bin'),
        '0xe000',str(framework/'tools/partitions/boot_app0.bin'),
        '0x10000',str(application)],check=True)
    manifest['variants'][variant] = {'version':json.loads((project/'generated'/ (name+'-version.json')).read_text()),
        'application':application.name,'factory':factory.name,'target':target.rstrip(b'\0').decode()}
manifest['frontend'] = json.loads((root/'frontend.lock.json').read_text())
manifest['reference_commit'] = '9519f2916563a4f2d1718b2f0db825169192af82'
(out/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n')
files = sorted([*out.glob('daikin-*.bin'),out/'manifest.json'])
(out/'SHA256SUMS').write_text(''.join(hashlib.sha256(f.read_bytes()).hexdigest()+'  '+f.name+'\n' for f in files))
print(out)

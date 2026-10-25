#!/usr/bin/env python3
"""Package the universal ESP32 image; factory images are flashed at address 0."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

project = Path(__file__).resolve().parent
root = project.parent
sys.path.insert(0, str(root / 'tools/release'))
from products import asset_stem
variant = os.environ.get('PHICOMM_FIRMWARE_VARIANT', 'full')
stem = asset_stem('daikin', variant)
out = root / ('build/daikin-release' if variant == 'full' else 'build/daikin-hub-release')
out.mkdir(parents=True, exist_ok=True)
for pattern in ('daikin-*.bin', 'phicomm_daikin*.bin', stem + '*.bin'):
    for stale in out.glob(pattern): stale.unlink()
core = Path(os.environ.get('PLATFORMIO_CORE_DIR', Path.home() / '.platformio'))
tool = core / 'packages/tool-esptoolpy/esptool.py'
framework = core / 'packages/framework-arduinoespressif32'
manifest = {'hardware_model':'BRY88AX151K','flash_size':4194304,'factory_offset':0,'firmware_variant':variant,'variants':{}}
for sensor_variant in ('auto',):
    name = 'bry88ab-hub' if variant == 'hub' else 'bry88ab'
    build = project / '.pio/build' / name
    app = (build / 'firmware.bin').read_bytes()
    target = b'BRY88AB151K'.ljust(32,b'\0')
    if (len(app) < 352 or app[0] != 0xe9 or
            struct.unpack_from('<I', app, 32)[0] != 0xabcd5432 or app[288:320] != target):
        raise RuntimeError('Missing or misplaced OTA model descriptor: ' + name)
    identities = {('PHICOMM-DAIKIN-1:' + v).encode().ljust(32,b'\0'):v for v in ('full','hub')}
    image_variant = identities.get(app[320:352])
    if image_variant != variant:
        raise RuntimeError('OTA firmware variant identity does not match package target: ' + name)
    application = out / (stem + '.ota.bin')
    shutil.copyfile(build / 'firmware.bin', application)
    factory = out / (stem + '.all.bin')
    subprocess.run([sys.executable,str(tool),'--chip','esp32','merge_bin',
        '--flash_mode','dio','--flash_freq','40m','--flash_size','4MB','-o',str(factory),
        '0x1000',str(build/'bootloader.bin'),'0x8000',str(build/'partitions.bin'),
        '0xe000',str(framework/'tools/partitions/boot_app0.bin'),
        '0x10000',str(application)],check=True)
    manifest['variants'][sensor_variant] = {'version':json.loads((project/'generated'/name/(name+'-version.json')).read_text()),
        'application':application.name,'factory':factory.name,'target':target.rstrip(b'\0').decode(),
        'firmware_variant':image_variant}
manifest['frontend'] = json.loads((project/'generated'/name/'frontend.json').read_text())
manifest['reference_commit'] = '9519f2916563a4f2d1718b2f0db825169192af82'
(out/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n')
files = sorted([*out.glob(stem + '*.bin'),out/'manifest.json'])
(out/'SHA256SUMS').write_text(''.join(hashlib.sha256(f.read_bytes()).hexdigest()+'  '+f.name+'\n' for f in files))
print(out)

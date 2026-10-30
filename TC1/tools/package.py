#!/usr/bin/env python3
"""Package the existing TC1 SDK's native images without changing their layout."""
import os
import hashlib
import json
import shutil
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from check_version import project_version, build_metadata

source, output = map(Path, sys.argv[1:3])
version = project_version()
version_metadata = build_metadata()
prefix = 'TC1@MK3031@moc'
variant = os.environ.get('PHICOMM_FIRMWARE_VARIANT', 'full')
if variant not in ('full', 'no-frontend-resources'): raise ValueError('Unknown variant')
artifact = "phicomm_tc1_a1" + ('' if variant == 'full' else '-no-frontend-resources')
output.mkdir(parents=True, exist_ok=True)
(output / (artifact + '.ota.bin.signed')).unlink(missing_ok=True)
sdk = json.loads((Path(__file__).resolve().parents[2] / 'common/mico-sdk.json').read_text())
manifest = {'version': version, 'target': prefix, 'sdk': sdk, 'sdk_commit': sdk['commit'],
            'toolchain': 'ARM GCC '+os.environ.get('PHICOMM_BUILD_COMPILER_EXPECTED', '5.4.1'), 'files': {}}
manifest.update(version_metadata)
manifest['firmware_variant'] = variant
manifest['frontend_http_queue_sha256'] = hashlib.sha256((Path(__file__).resolve().parents[2] / 'common/web/http_queue.js').read_bytes()).hexdigest()
frontend_root = Path(__file__).resolve().parents[2]
frontend_metadata = frontend_root / 'build/frontend-tc1-provenance.json'
manifest['frontend'] = json.loads((frontend_metadata if frontend_metadata.exists() else frontend_root / 'frontend.lock.json').read_text())
for suffix in ('.usr.bin', '.ota.bin', '.all.bin', '.elf', '.map'):
    original = source / (prefix + suffix)
    destination = output / (artifact + suffix)
    shutil.copyfile(original, destination)
    manifest['files'][destination.name] = hashlib.sha256(destination.read_bytes()).hexdigest()
if (version.encode() + b'\0') not in (output / (artifact + '.usr.bin')).read_bytes():
    raise SystemExit('Built image does not contain project version')
image = (output / (artifact + '.ota.bin')).read_bytes()
if hashlib.md5(image[:-16]).digest() != image[-16:]:
    raise SystemExit('TC1 OTA MD5 mismatch')
(output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
(output / 'SHA256SUMS').write_text(''.join(f'{digest}  {name}\n' for name, digest in manifest['files'].items()))
print(f'TC1 {version} artifacts: {output}')

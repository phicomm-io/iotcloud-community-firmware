#!/usr/bin/env python3
"""Package the existing TC1 SDK's native images without changing their layout."""
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
output.mkdir(parents=True, exist_ok=True)
sdk = json.loads((Path(__file__).resolve().parents[1] / 'sdk.json').read_text())
manifest = {'version': version, 'target': prefix, 'sdk': sdk,
            'toolchain': 'ARM GCC 5.4.1', 'files': {}}
manifest.update(version_metadata)
for suffix in ('.usr.bin', '.ota.bin', '.all.bin', '.elf', '.map'):
    original = source / (prefix + suffix)
    destination = output / ('phicomm_tc1_a1' + suffix)
    shutil.copyfile(original, destination)
    manifest['files'][destination.name] = hashlib.sha256(destination.read_bytes()).hexdigest()
if (version.encode() + b'\0') not in (output / 'phicomm_tc1_a1.usr.bin').read_bytes():
    raise SystemExit('Built image does not contain project version')
image = (output / 'phicomm_tc1_a1.ota.bin').read_bytes()
if hashlib.md5(image[:-16]).digest() != image[-16:]:
    raise SystemExit('TC1 OTA MD5 mismatch')
(output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
(output / 'SHA256SUMS').write_text(''.join(f'{digest}  {name}\n' for name, digest in manifest['files'].items()))
print(f'TC1 {version} artifacts: {output}')

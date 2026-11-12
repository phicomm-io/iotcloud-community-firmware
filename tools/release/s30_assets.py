#!/usr/bin/env python3
"""Collect verified native S30 files; retain Git-derived on-device identity."""
import argparse
import hashlib
import json
from pathlib import Path
import re

from release_version import VERSION_PATTERN


def collect(run, output):
    manifest = json.loads((run / 'build-manifest.json').read_bytes())
    validation = json.loads((run / 'firmware-validation.json').read_bytes())
    metadata = manifest['version_metadata']
    version, base, commit = manifest['version'], metadata['base_version'], manifest['project_head']
    if (not isinstance(base, str) or not VERSION_PATTERN.fullmatch(base) or
            manifest.get('dirty') is not False or metadata.get('dirty') is not False or
            not re.fullmatch('[0-9a-f]{40}', commit) or metadata.get('git_commit') != commit or
            version != metadata.get('version') or version != base + '-' + commit[:6]):
        raise ValueError('S30 release requires clean, coherent Git-derived build identity')
    if (manifest.get('flash_layout', {}).get('name') != 's30-v1b-stock-v1' or
            manifest['flash_layout'].get('bootloader_included') is not False or
            validation.get('status') != 'PASS'):
        raise ValueError('S30 release requires the verified stock bootloader layout')
    assets = {}
    checks = {}
    for role, suffix in (('OTA', 'rbl'), ('STOCK_UA', 'bin'), ('app', 'bin'), ('UG', 'bin')):
        source = f'BL2028N_{role}_{version}.{suffix}'
        name = f'BL2028N_{role}_{base}.{suffix}'
        body = (run / 'firmware' / source).read_bytes()
        digest = hashlib.sha256(body).hexdigest()
        if not body:
            raise ValueError('Empty S30 firmware: ' + source)
        checks[source] = digest
        assets[name] = body
    expected = dict(line.split('  ', 1)[::-1] for line in
                    (run / 'firmware/SHA256SUMS').read_text().splitlines())
    if checks != expected:
        raise ValueError('Native S30 build checksums differ or include unexpected files')
    manifest['release_files'] = {
        name: {'sha256': hashlib.sha256(body).hexdigest(), 'size': len(body)}
        for name, body in assets.items()}
    assets['build-s30.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    assets['validation-s30.json'] = (json.dumps(validation, indent=2) + '\n').encode()
    output.mkdir(parents=True, exist_ok=True)
    for name, body in assets.items():
        target = output / name
        if target.exists() and target.read_bytes() != body:
            raise ValueError('Refuse to replace a different native S30 asset: ' + name)
    for name, body in assets.items():
        (output / name).write_bytes(body)
    (output / 'SHA256SUMS-S30').write_text(''.join(
        hashlib.sha256(body).hexdigest() + '  ' + name + '\n'
        for name, body in sorted(assets.items())))
    print('Collected native S30 files:', ', '.join(sorted(assets)))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    collect(args.run, args.output)

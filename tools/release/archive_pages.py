#!/usr/bin/env python3
"""Copy the published catalog and hash-verified firmware to static hosting."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
from urllib.request import Request, urlopen


def write_latest(catalog, output):
    """Small device manifests: follow stable, and expose signed images only."""
    stable = catalog.get('stable')
    release = next((r for r in catalog['releases'] if r['tag'] == stable), None)
    if stable and release is None:
        raise ValueError('Stable release is absent from catalog')
    for model in ('tc1', 'm1', 'a1'):
        item = release.get('models', {}).get(model, {}) if release else {}
        for variant, entry in [('full', item), ('no-frontend-resources', item.get('variants', {}).get('no-frontend-resources', {}))]:
            folder = output / 'ota' / model
            if variant != 'full': folder /= variant
            target = folder / 'latest.json'
            signed = entry.get('downloads', {}).get('signed')
            if not signed:
                target.unlink(missing_ok=True)
                continue
            document = dict(schema=1, tag=release['tag'], version=release['version'],
                            commit=release['commit'], model=model, firmware_variant=variant,
                            **signed, release_log=release['notes'])
            data = (json.dumps(document, ensure_ascii=False, separators=(',', ':')) + '\n').encode()
            if len(data) > 4096: raise ValueError('Device manifest exceeds 4096 bytes')
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)


def mirror(source, output):
    output = Path(output)
    def fetch(path):
        parts = PurePosixPath(path)
        if not path.startswith('/releases/') or '..' in parts.parts:
            raise ValueError(f'Invalid release path: {path}')
        with urlopen(Request(source.rstrip('/') + path, headers={'User-Agent': 'curl/8.5.0'}), timeout=60) as response:
            return response.read()
    catalog_bytes = fetch('/releases/index.json')
    catalog = json.loads(catalog_bytes)
    for release in catalog['releases']:
        assets = {}
        for model in release['models'].values():
            for entry in [model, *model.get('variants', {}).values()]:
                for value in entry['downloads'].values():
                    assets[value['path']] = value
        for path, value in assets.items():
            target = output / path.lstrip('/')
            data = target.read_bytes() if target.exists() else b''
            if hashlib.sha256(data).hexdigest() != value['sha256']:
                data = fetch(path)
            if len(data) != value['size'] or hashlib.sha256(data).hexdigest() != value['sha256']:
                raise ValueError(f'Firmware checksum mismatch: {path}')
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        for path in (release['notes'], f"/releases/{release['tag']}/SHA256SUMS"):
            target = output / path.lstrip('/')
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(fetch(path))
    target = output / 'releases/index.json'
    target.parent.mkdir(parents=True, exist_ok=True)
    write_latest(catalog, output)
    target.write_bytes(catalog_bytes)
    print(f"Archived {len(catalog['releases'])} releases with verified firmware hashes")


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output')
    parser.add_argument('--source', default='https://phicomm.fjrcn.com')
    parser.add_argument('--existing', action='store_true', help='Regenerate device manifests from the retained local release catalog')
    args = parser.parse_args()
    if args.existing:
        output = Path(args.output)
        write_latest(json.loads((output / 'releases/index.json').read_text()), output)
    else:
        mirror(args.source, args.output)

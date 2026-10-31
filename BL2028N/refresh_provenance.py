#!/usr/bin/env python3
"""Pin portable adaptations against clean shared sources and a verified Paho base."""
import argparse
import difflib
import gzip
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile

HERE = Path(__file__).resolve().parent
FIRMWARE = HERE.parent
HISTORICAL_SHA = 'd646deb4bed3c14cd219f467b005f88214a0f6b8857049f51fada8520b3bcd9e'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frontend-root', type=Path, required=True)
    parser.add_argument('--historical-snapshot', type=Path, required=True)
    parser.add_argument('--snapshot-output', type=Path, required=True)
    args = parser.parse_args()
    frontend = args.frontend_root.resolve()
    old = args.historical_snapshot.read_bytes()
    if digest(old) != HISTORICAL_SHA:
        raise SystemExit('Historical shared-source snapshot checksum mismatch')
    historical = tarfile.open(fileobj=io.BytesIO(old), mode='r:gz')
    manifest_path = HERE / 'protocol/lab-shared-source.json'
    manifest = json.loads(manifest_path.read_text())
    manifest['files'].pop('web/shared/wire.js', None)
    manifest.pop('wire_bundle_sha256', None)
    manifest['files']['protocol/diagnostics_ids.h'] = {'source': 'common/diagnostics_ids.h'}
    manifest['files']['protocol/mqtt/discovery_mp.h'] = {'source': 'common/mqtt/discovery_mp.h'}
    manifest['files']['protocol/httpd/basic.h'] = {'source': 'common/httpd/basic.h'}
    for name in ('lan_policy.h', 'lan_auth.inc', 'lan_wire_v4.h', 'lan_wire_v4.inc'):
        manifest['files']['common/' + name] = {'source': 'common/' + name}
    manifest['files']['web/index.html'] = {'source': 'frontend/S30/web/index.html'}
    manifest['files']['web/page.js'] = {'source': 'frontend/S30/web/page.js'}
    manifest['files']['web/shared/diagnostics-schema.js'] = {'source': 'frontend/common/web/diagnostics-schema.js'}
    patches = {'protocol': [], 'web': []}
    snapshot = {}
    current_paths = []
    for dest, item in manifest['files'].items():
        source = item['source']
        if source.startswith('frontend/'):
            base = (frontend / source.removeprefix('frontend/')).read_bytes()
            snapshot[source] = base
        elif source.startswith('paho/') or not (FIRMWARE / source).exists():
            name = source if source.startswith('paho/') else 'lab/' + source
            raw = historical.extractfile(name).read()
            snapshot[name] = raw
            base = raw.split(b'static bool feature_parse(')[0] if item.get('extraction') else raw
            item['historical_source_snapshot_sha256'] = HISTORICAL_SHA
        else:
            base = (FIRMWARE / source).read_bytes()
            snapshot['lab/' + source] = base
            current_paths.append(source)
        adapted = (HERE / dest).read_bytes()
        item['upstream_sha256'] = digest(base)
        item['sha256'] = digest(adapted)
        if item.get('extraction'):
            item['base_sha256'] = digest(base)
        patch_group = 'web' if dest.startswith('web/') else 'protocol'
        patches[patch_group].extend(difflib.unified_diff(
            base.decode().splitlines(True), adapted.decode().splitlines(True),
            fromfile='a/' + dest, tofile='b/' + dest))
    # The auth bundle carries the actual binary codec and device discovery.
    auth_sources = ['tools/bundle_auth.cjs', 'frontend/src/deviceAuth.js',
                    'frontend/src/deviceWire.js', 'frontend/src/wireCodec.js',
                    'common/web/auth.js']
    manifest['auth_bundle_sources'] = {}
    for name in auth_sources:
        raw = (frontend / name).read_bytes()
        snapshot['frontend/' + name] = raw
        manifest['auth_bundle_sources'][name] = digest(raw)
    for rel in ('common/mqtt-receive-recovery.patch', 'common/mqtt-post-send-workspace.patch'):
        raw = (FIRMWARE / rel).read_bytes()
        snapshot['lab/' + rel] = raw
        manifest['paho_patch_stack'][rel] = digest(raw)
    for key, root, paths in [('lab', FIRMWARE, current_paths), ('frontend', frontend, ['common/web', 'S30/web', *auth_sources])]:
        # Only adaptation files may be dirty, never the pinned upstream inputs.
        if subprocess.run(['git', 'diff', '--quiet', 'HEAD', '--', *paths], cwd=root).returncode:
            raise SystemExit('Commit shared inputs before pinning: ' + key)
        manifest[key] = {'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
                         'dirty': False, 'scope': 'listed upstream source paths'}
    for key, rel in [('protocol', 'protocol/lab-openbeken.patch'), ('web', 'web/frontend-openbeken.patch')]:
        raw = ''.join(patches[key]).encode()
        (HERE / rel).write_bytes(raw)
        manifest['adaptation_patch_sha256'][rel] = digest(raw)
    for path in sorted((HERE / 'patches').glob('*.patch')):
        manifest['adaptation_patch_sha256'][str(path.relative_to(HERE))] = digest(path.read_bytes())
    archive = io.BytesIO()
    with gzip.GzipFile(fileobj=archive, mode='wb', mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode='w') as tar:
            for name, data in sorted(snapshot.items()):
                info = tarfile.TarInfo(name); info.size = len(data); info.mode = 0o644
                tar.addfile(info, io.BytesIO(data))
    args.snapshot_output.parent.mkdir(parents=True, exist_ok=True)
    args.snapshot_output.write_bytes(archive.getvalue())
    manifest['source_snapshot'] = {'file': args.snapshot_output.name, 'sha256': digest(archive.getvalue()), 'entries': len(snapshot)}
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    provenance_path = HERE / 'web/shared/provenance.json'
    provenance = json.loads(provenance_path.read_text())
    provenance.update(commit=manifest['frontend']['head'], source_dirty=False)
    provenance['sha256'] = {p.name: digest(p.read_bytes()) for p in sorted((HERE / 'web/shared').iterdir()) if p.suffix in ('.js', '.css') and p.name != 'wire.js'}
    provenance['adaptations'] = 'Shared binary auth, serialized reads, diagnostics and product capability gates; see frontend-openbeken.patch and lab-shared-source.json.'
    provenance_path.write_text(json.dumps(provenance, indent=2) + '\n')
    (HERE / 'web/shared/wire.js').unlink(missing_ok=True)
    typed_path = HERE / 'protocol/lab-source.json'
    typed = json.loads(typed_path.read_text())
    typed.update(source='phicomm-community-firmware/common', git_head=manifest['lab']['head'],
                 note='Clean shared inputs with portable binary-only adaptations recorded in lab-openbeken.patch.')
    typed['files'] = {name: digest((HERE / 'protocol' / name).read_bytes()) for name in typed['files']}
    typed_path.write_text(json.dumps(typed, indent=2) + '\n')
    print('Pinned firmware', manifest['lab']['head'], 'frontend', manifest['frontend']['head'])


if __name__ == '__main__':
    main()
